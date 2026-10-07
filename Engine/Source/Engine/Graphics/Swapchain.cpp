#include "EnginePCH.h"
#include "Engine/Graphics/Swapchain.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Window.h"

// Order matters: glfw3.h declares glfwCreateWindowSurface only when the Vulkan headers come first (ADR 0005 decision 8).
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <format>
#include <limits>
#include <string>
#include <utility>

// Every Vulkan call here goes through the dispatcher's C entry points and checks its VkResult (Architecture §8.1, §4.6
// item 2): nothing in this file throws.

namespace Engine {

	namespace Utils {

		static std::string_view PresentModeToString(VkPresentModeKHR mode)
		{
			switch (mode)
			{
				case VK_PRESENT_MODE_FIFO_KHR:      return "FIFO";
				case VK_PRESENT_MODE_MAILBOX_KHR:   return "MAILBOX";
				case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
				default:                            return "other";
			}
		}

		// Fills `items` through a two-call Vulkan enumeration, repeating while the count changes in between (VK_INCOMPLETE).
		template<typename T, typename Enumerate>
		static VkResult EnumerateVulkanArray(std::vector<T>& items, const Enumerate& enumerate)
		{
			for (;;)
			{
				uint32_t count = 0;
				VkResult result = enumerate(&count, nullptr);
				if (result != VK_SUCCESS)
					return result;
				items.resize(count);
				result = enumerate(&count, items.data());
				if (result == VK_INCOMPLETE)
					continue;
				items.resize(count);
				return result;
			}
		}

		// B8G8R8A8_UNORM, else R8G8B8A8_UNORM, with sRGB-encoded values in a UNORM image (§8.9); nullopt when the surface
		// offers neither. A single VK_FORMAT_UNDEFINED entry means that the surface takes any format.
		static std::optional<VkSurfaceFormatKHR> ChooseSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& formats)
		{
			const VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
			if (formats.size() == 1 && formats[0].format == VK_FORMAT_UNDEFINED)
				return VkSurfaceFormatKHR{ VK_FORMAT_B8G8R8A8_UNORM, colorSpace };
			for (const VkFormat preferred : { VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM })
			{
				const auto found = std::ranges::find_if(formats, [preferred](const VkSurfaceFormatKHR& format)
				{
					return format.format == preferred && format.colorSpace == colorSpace;
				});
				if (found != formats.end())
					return *found;
			}
			return std::nullopt;
		}

		// With VSync FIFO (always supported), else MAILBOX, else IMMEDIATE (§8.1); without it MAILBOX, then IMMEDIATE, then
		// FIFO (GraphicsSpecification::VSync).
		static VkPresentModeKHR ChoosePresentMode(const std::vector<VkPresentModeKHR>& modes, bool vsync)
		{
			const std::array<VkPresentModeKHR, 3> withVSync = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR };
			const std::array<VkPresentModeKHR, 3> withoutVSync = { VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR };
			for (const VkPresentModeKHR candidate : vsync ? withVSync : withoutVSync)
			{
				if (std::ranges::find(modes, candidate) != modes.end())
					return candidate;
			}
			return VK_PRESENT_MODE_FIFO_KHR;
		}

		static VkCompositeAlphaFlagBitsKHR ChooseCompositeAlpha(VkCompositeAlphaFlagsKHR supported)
		{
			for (const VkCompositeAlphaFlagBitsKHR candidate : { VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
					 VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR })
			{
				if ((supported & candidate) != 0)
					return candidate;
			}
			return VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		}

		static Result<VkSemaphore> CreateBinarySemaphore(VkDevice device)
		{
			const VkSemaphoreCreateInfo createInfo = {
				.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
				.pNext = nullptr,
				.flags = 0,
			};
			VkSemaphore semaphore = VK_NULL_HANDLE;
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device, &createInfo, nullptr, &semaphore);
			if (result != VK_SUCCESS)
				return MakeError(ErrorCode::Gpu, "vkCreateSemaphore failed with {}", VkResultToString(result));
			return semaphore;
		}

	}

	SwapchainAction ClassifyAcquireResult(VkResult result)
	{
		switch (result)
		{
			case VK_SUCCESS:                    return SwapchainAction::Continue;
			case VK_SUBOPTIMAL_KHR:             return SwapchainAction::ContinueThenRecreate;
			case VK_ERROR_OUT_OF_DATE_KHR:      return SwapchainAction::Recreate;
			case VK_ERROR_SURFACE_LOST_KHR:     return SwapchainAction::RecreateSurface;
			case VK_TIMEOUT:                    return SwapchainAction::Retry;
			case VK_NOT_READY:                  return SwapchainAction::Retry;
			case VK_ERROR_DEVICE_LOST:          return SwapchainAction::DeviceLost;
			case VK_ERROR_OUT_OF_HOST_MEMORY:   return SwapchainAction::OutOfMemory;
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return SwapchainAction::OutOfMemory;
			default:                            return SwapchainAction::Fail;
		}
	}

	SwapchainAction ClassifyPresentResult(VkResult result)
	{
		switch (result)
		{
			case VK_SUCCESS:                    return SwapchainAction::Continue;
			case VK_SUBOPTIMAL_KHR:             return SwapchainAction::Recreate;
			case VK_ERROR_OUT_OF_DATE_KHR:      return SwapchainAction::Recreate;
			case VK_ERROR_SURFACE_LOST_KHR:     return SwapchainAction::RecreateSurface;
			case VK_ERROR_DEVICE_LOST:          return SwapchainAction::DeviceLost;
			case VK_ERROR_OUT_OF_HOST_MEMORY:   return SwapchainAction::OutOfMemory;
			case VK_ERROR_OUT_OF_DEVICE_MEMORY: return SwapchainAction::OutOfMemory;
			default:                            return SwapchainAction::Fail;
		}
	}

	Swapchain::Swapchain(ConstructionKey /*key*/)
	{
	}

	Swapchain::~Swapchain()
	{
		// A swapchain whose Create failed before it had a device owns nothing.
		if (m_Device == nullptr)
			return;

		// §8.14 item 4: the swapchain goes after the GPU is idle and before the NVRHI device.
		m_Device->WaitForIdle();
		ReleaseImages();
		DestroySwapchain();
		const VkDevice device = m_Device->GetVulkanDevice();
		for (const VkSemaphore semaphore : m_AcquireSemaphores)
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(device, semaphore, nullptr);
		m_AcquireSemaphores.clear();
		DestroySurface();
	}

	Result<Scope<Swapchain>> Swapchain::Create(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification)
	{
		ENGINE_CORE_VERIFY(specification.FramesInFlight >= 1, "Swapchain needs at least one frame in flight");
		// A headless device has no swapchain entry points: an error, never a call through a null pointer.
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSwapchainKHR == nullptr
			|| VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceCapabilitiesKHR == nullptr)
		{
			return MakeError(ErrorCode::InvalidState,
				"cannot create a swapchain: the device was created without presentation support (GraphicsDeviceSpecification::PresentWindow)");
		}

		Scope<Swapchain> swapchain = CreateScope<Swapchain>(ConstructionKey());
		swapchain->m_Device = &device;
		swapchain->m_Window = &window;
		swapchain->m_Specification = specification;

		const VkDevice vulkanDevice = device.GetVulkanDevice();
		for (uint32_t slot = 0; slot < specification.FramesInFlight; ++slot)
		{
			ENGINE_TRY_ASSIGN(const VkSemaphore semaphore,
				WithContext(Utils::CreateBinarySemaphore(vulkanDevice), "while creating the swapchain's acquire semaphores"));
			swapchain->m_AcquireSemaphores.push_back(semaphore);
		}

		ENGINE_TRY(swapchain->CreateSurface());
		if (!window.IsMinimized() && window.GetFramebufferWidth() > 0 && window.GetFramebufferHeight() > 0)
			ENGINE_TRY(swapchain->CreateSwapchain(window.GetFramebufferWidth(), window.GetFramebufferHeight()));

		if (swapchain->m_Swapchain != VK_NULL_HANDLE)
		{
			ENGINE_CORE_INFO("Swapchain created: {}x{} {}, {} present mode, {} images", swapchain->m_Width, swapchain->m_Height,
				swapchain->m_Format == nvrhi::Format::BGRA8_UNORM ? "B8G8R8A8_UNORM" : "R8G8B8A8_UNORM",
				Utils::PresentModeToString(swapchain->m_PresentMode), swapchain->m_Images.size());
		}
		else
		{
			ENGINE_CORE_INFO("Swapchain: the window is minimized; its images are created when it is restored");
		}
		return swapchain;
	}

	Result<SwapchainAcquireStatus> Swapchain::AcquireNextImage(uint32_t frameSlot)
	{
		ENGINE_CORE_VERIFY(frameSlot < m_AcquireSemaphores.size(), "Swapchain::AcquireNextImage frame slot {} is out of range ({} frames in flight)",
			frameSlot, m_AcquireSemaphores.size());
		ENGINE_CORE_ASSERT(!m_HasAcquiredImage, "Swapchain::AcquireNextImage called again before Present");
		m_HasAcquiredImage = false;

		// Minimized (iconified, or a framebuffer with a zero dimension): nothing to render or present (§4.2). The images
		// are released while the window stays minimized and created again at the restored size.
		const uint32_t width = m_Window->GetFramebufferWidth();
		const uint32_t height = m_Window->GetFramebufferHeight();
		if (m_Window->IsMinimized() || width == 0 || height == 0)
		{
			if (m_Swapchain != VK_NULL_HANDLE)
			{
				m_Device->WaitForIdle();
				ReleaseImages();
				DestroySwapchain();
				ENGINE_CORE_TRACE("Swapchain released while the window is minimized");
			}
			return SwapchainAcquireStatus::Skipped;
		}

		// Resize events, out-of-date and suboptimal results only mark a recreation; it happens here, once, at the size the
		// framebuffer has now, however many of them arrived since the last frame.
		if (m_Swapchain == VK_NULL_HANDLE || m_IsRecreatePending || m_IsSurfaceLost || width != m_RequestedWidth || height != m_RequestedHeight)
		{
			ENGINE_TRY(Recreate(width, height));
			return SwapchainAcquireStatus::Skipped;
		}

		VkResult result = VK_SUCCESS;
		uint32_t imageIndex = 0;
		if (m_InjectedAcquireResult == VK_ERROR_OUT_OF_DATE_KHR || m_InjectedAcquireResult == VK_ERROR_SURFACE_LOST_KHR)
		{
			// These results acquire no image, so they replace the call itself (InjectAcquireResultForTesting).
			result = *m_InjectedAcquireResult;
			m_InjectedAcquireResult.reset();
		}
		else
		{
			// Bounded: one wait slice at most (FramePacer.h), never an unbounded wait on the presentation engine (§8.1).
			result = VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR(m_Device->GetVulkanDevice(), m_Swapchain, GpuWaitSliceNanoseconds,
				m_AcquireSemaphores[frameSlot], VK_NULL_HANDLE, &imageIndex);
			if (result == VK_SUCCESS && m_InjectedAcquireResult == VK_SUBOPTIMAL_KHR)
			{
				result = VK_SUBOPTIMAL_KHR;
				m_InjectedAcquireResult.reset();
			}
		}

		const SwapchainAction action = ClassifyAcquireResult(result);
		switch (action)
		{
			case SwapchainAction::Continue:
			case SwapchainAction::ContinueThenRecreate:
			{
				ENGINE_CORE_VERIFY(imageIndex < m_Images.size(), "vkAcquireNextImageKHR returned image {} of {}", imageIndex, m_Images.size());
				m_CurrentImage = imageIndex;
				m_CurrentSlot = frameSlot;
				m_HasAcquiredImage = true;
				m_IsAcquireTimingOut = false;
				if (action == SwapchainAction::ContinueThenRecreate)
					m_IsRecreatePending = true;
				return SwapchainAcquireStatus::Acquired;
			}
			case SwapchainAction::Recreate:
			{
				m_IsRecreatePending = true;
				ENGINE_TRY(Recreate(width, height));
				return SwapchainAcquireStatus::Skipped;
			}
			case SwapchainAction::RecreateSurface:
			{
				ENGINE_CORE_WARN("vkAcquireNextImageKHR reported the window's surface lost; recreating the surface and the swapchain");
				m_IsSurfaceLost = true;
				ENGINE_TRY(Recreate(width, height));
				return SwapchainAcquireStatus::Skipped;
			}
			case SwapchainAction::Retry:
			{
				// The presentation engine holds its images back (a hidden or occluded window); a stopped GPU is caught by
				// FramePacer's bounded waits instead.
				if (!m_IsAcquireTimingOut)
				{
					m_IsAcquireTimingOut = true;
					ENGINE_CORE_WARN("vkAcquireNextImageKHR returned no image within {} ms ({}); skipping frames until one is available",
						GpuWaitSliceNanoseconds / 1'000'000, VkResultToString(result));
				}
				return SwapchainAcquireStatus::Skipped;
			}
			case SwapchainAction::DeviceLost:
			case SwapchainAction::OutOfMemory:
			case SwapchainAction::Fail:
			{
				EndProcess("vkAcquireNextImageKHR", result);
			}
		}

		ENGINE_CORE_ASSERT(false, "Unknown SwapchainAction {}", std::to_underlying(action));
		EndProcess("vkAcquireNextImageKHR", result);
	}

	void Swapchain::QueueFrameSemaphores()
	{
		ENGINE_CORE_VERIFY(m_HasAcquiredImage, "Swapchain::QueueFrameSemaphores without an acquired image");
		// Binary semaphores: the value is ignored (0). The frame's last submission waits until the image is acquired and
		// signals the image's present semaphore, which the present waits on.
		m_Device->QueueWaitForSemaphore(m_AcquireSemaphores[m_CurrentSlot], 0);
		m_Device->QueueSignalSemaphore(m_Images[m_CurrentImage].PresentSemaphore, 0);
	}

	void Swapchain::Present()
	{
		ENGINE_CORE_VERIFY(m_HasAcquiredImage, "Swapchain::Present without an acquired image");
		m_HasAcquiredImage = false;

		const VkSemaphore waitSemaphore = m_Images[m_CurrentImage].PresentSemaphore;
		const VkPresentInfoKHR presentInfo = {
			.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
			.pNext = nullptr,
			.waitSemaphoreCount = 1,
			.pWaitSemaphores = &waitSemaphore,
			.swapchainCount = 1,
			.pSwapchains = &m_Swapchain,
			.pImageIndices = &m_CurrentImage,
			.pResults = nullptr,
		};
		VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkQueuePresentKHR(m_Device->GetGraphicsQueue(), &presentInfo);
		if (m_InjectedPresentResult.has_value())
		{
			// The image was presented; the injected result replaces only what the call returned.
			result = *m_InjectedPresentResult;
			m_InjectedPresentResult.reset();
		}

		const SwapchainAction action = ClassifyPresentResult(result);
		switch (action)
		{
			case SwapchainAction::Continue:
			{
				return;
			}
			case SwapchainAction::Recreate:
			{
				m_IsRecreatePending = true;
				return;
			}
			case SwapchainAction::RecreateSurface:
			{
				ENGINE_CORE_WARN("vkQueuePresentKHR reported the window's surface lost; the next frame recreates the surface and the swapchain");
				m_IsSurfaceLost = true;
				return;
			}
			case SwapchainAction::ContinueThenRecreate:
			case SwapchainAction::Retry:
			case SwapchainAction::DeviceLost:
			case SwapchainAction::OutOfMemory:
			case SwapchainAction::Fail:
			{
				// ClassifyPresentResult never returns ContinueThenRecreate or Retry.
				EndProcess("vkQueuePresentKHR", result);
			}
		}

		ENGINE_CORE_ASSERT(false, "Unknown SwapchainAction {}", std::to_underlying(action));
		EndProcess("vkQueuePresentKHR", result);
	}

	void Swapchain::RequestRecreate()
	{
		m_IsRecreatePending = true;
	}

	void Swapchain::InjectAcquireResultForTesting(VkResult result)
	{
		ENGINE_CORE_ASSERT(result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR,
			"Swapchain::InjectAcquireResultForTesting takes VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR or VK_ERROR_SURFACE_LOST_KHR, got {}",
			std::to_underlying(result));
		m_InjectedAcquireResult = result;
	}

	void Swapchain::InjectPresentResultForTesting(VkResult result)
	{
		ENGINE_CORE_ASSERT(result == VK_SUBOPTIMAL_KHR || result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_ERROR_SURFACE_LOST_KHR,
			"Swapchain::InjectPresentResultForTesting takes VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR or VK_ERROR_SURFACE_LOST_KHR, got {}",
			std::to_underlying(result));
		m_InjectedPresentResult = result;
	}

	nvrhi::ITexture* Swapchain::GetCurrentTexture() const
	{
		return m_HasAcquiredImage ? m_Images[m_CurrentImage].Texture.Get() : nullptr;
	}

	nvrhi::IFramebuffer* Swapchain::GetCurrentFramebuffer() const
	{
		return m_HasAcquiredImage ? m_Images[m_CurrentImage].Framebuffer.Get() : nullptr;
	}

	nvrhi::Format Swapchain::GetFormat() const
	{
		return m_Format;
	}

	uint32_t Swapchain::GetWidth() const
	{
		return m_Width;
	}

	uint32_t Swapchain::GetHeight() const
	{
		return m_Height;
	}

	uint32_t Swapchain::GetImageCount() const
	{
		return static_cast<uint32_t>(m_Images.size());
	}

	VkPresentModeKHR Swapchain::GetPresentMode() const
	{
		return m_PresentMode;
	}

	bool Swapchain::IsMinimized() const
	{
		return m_Swapchain == VK_NULL_HANDLE;
	}

	uint32_t Swapchain::GetRecreationCount() const
	{
		return m_RecreationCount;
	}

	Status Swapchain::CreateSurface()
	{
		ENGINE_CORE_ASSERT(m_Surface == VK_NULL_HANDLE, "Swapchain::CreateSurface with a surface");
		VkSurfaceKHR surface = VK_NULL_HANDLE;
		VkResult result = glfwCreateWindowSurface(m_Device->GetVulkanInstance(), static_cast<GLFWwindow*>(m_Window->GetNativeHandle()), nullptr, &surface);
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "glfwCreateWindowSurface failed with {}", VkResultToString(result));
		m_Surface = surface;

		// The device was chosen for a queue family that can present to this window (GraphicsDevice::Create); the surface
		// itself is checked too, as Vulkan requires before a swapchain is created on it.
		const uint32_t queueFamily = m_Device->GetInfo().GraphicsQueueFamily;
		VkBool32 isSupported = VK_FALSE;
		result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceSupportKHR(m_Device->GetVulkanPhysicalDevice(), queueFamily, m_Surface, &isSupported);
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkGetPhysicalDeviceSurfaceSupportKHR failed with {}", VkResultToString(result));
		if (isSupported != VK_TRUE)
			return MakeError(ErrorCode::Unsupported, "the graphics queue family {} cannot present to the window's surface", queueFamily);
		return {};
	}

	Status Swapchain::CreateSwapchain(uint32_t width, uint32_t height)
	{
		ENGINE_CORE_ASSERT(m_Images.empty(), "Swapchain::CreateSwapchain with images still held");
		const VkPhysicalDevice physicalDevice = m_Device->GetVulkanPhysicalDevice();
		const VkDevice device = m_Device->GetVulkanDevice();

		VkSurfaceCapabilitiesKHR capabilities{};
		VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, m_Surface, &capabilities);
		if (result == VK_ERROR_SURFACE_LOST_KHR)
		{
			// The next AcquireNextImage creates a new surface first.
			ENGINE_CORE_WARN("The window's surface was lost while the swapchain was created; it is recreated with the next frame");
			m_IsSurfaceLost = true;
			DestroySwapchain();
			return {};
		}
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed with {}", VkResultToString(result));

		// A surface with a fixed extent dictates it; otherwise the framebuffer size, within the surface's limits.
		VkExtent2D extent = capabilities.currentExtent;
		if (extent.width == std::numeric_limits<uint32_t>::max())
		{
			extent.width = std::clamp(width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
			extent.height = std::clamp(height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);
		}
		if (extent.width == 0 || extent.height == 0)
		{
			// The surface reports a minimized window although the window has not told yet: no swapchain until it does.
			DestroySwapchain();
			return {};
		}

		std::vector<VkSurfaceFormatKHR> formats;
		result = Utils::EnumerateVulkanArray(formats, [physicalDevice, this](uint32_t* count, VkSurfaceFormatKHR* items)
		{
			return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, m_Surface, count, items);
		});
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkGetPhysicalDeviceSurfaceFormatsKHR failed with {}", VkResultToString(result));
		const std::optional<VkSurfaceFormatKHR> surfaceFormat = Utils::ChooseSurfaceFormat(formats);
		if (!surfaceFormat.has_value())
			return MakeError(ErrorCode::Unsupported, "the window's surface offers neither B8G8R8A8_UNORM nor R8G8B8A8_UNORM in the sRGB color space");

		std::vector<VkPresentModeKHR> presentModes;
		result = Utils::EnumerateVulkanArray(presentModes, [physicalDevice, this](uint32_t* count, VkPresentModeKHR* items)
		{
			return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, m_Surface, count, items);
		});
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkGetPhysicalDeviceSurfacePresentModesKHR failed with {}", VkResultToString(result));
		const VkPresentModeKHR presentMode = Utils::ChoosePresentMode(presentModes, m_Specification.VSync);

		// Frames clear their target with a transfer (FrameClearColor) and render into it; reading it back is a bonus.
		if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT) == 0)
			return MakeError(ErrorCode::Unsupported, "the window's surface does not support swapchain images that can be cleared (transfer destination)");
		VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		if ((capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0)
			usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

		// One image more than the minimum, so the CPU never waits for the presentation engine to release one.
		uint32_t imageCount = capabilities.minImageCount + 1;
		if (capabilities.maxImageCount != 0)
			imageCount = std::min(imageCount, capabilities.maxImageCount);

		const VkSwapchainCreateInfoKHR createInfo = {
			.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
			.pNext = nullptr,
			.flags = 0,
			.surface = m_Surface,
			.minImageCount = imageCount,
			.imageFormat = surfaceFormat->format,
			.imageColorSpace = surfaceFormat->colorSpace,
			.imageExtent = extent,
			.imageArrayLayers = 1,
			.imageUsage = usage,
			.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
			.queueFamilyIndexCount = 0,
			.pQueueFamilyIndices = nullptr,
			.preTransform = capabilities.currentTransform,
			.compositeAlpha = Utils::ChooseCompositeAlpha(capabilities.supportedCompositeAlpha),
			.presentMode = presentMode,
			.clipped = VK_TRUE,
			.oldSwapchain = m_Swapchain,
		};
		VkSwapchainKHR created = VK_NULL_HANDLE;
		result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSwapchainKHR(device, &createInfo, nullptr, &created);
		// The old swapchain is retired by the call whether or not it succeeded, and its images are released already.
		DestroySwapchain();
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkCreateSwapchainKHR ({}x{}) failed with {}", extent.width, extent.height, VkResultToString(result));
		m_Swapchain = created;
		m_Width = extent.width;
		m_Height = extent.height;
		m_RequestedWidth = width;
		m_RequestedHeight = height;
		m_Format = surfaceFormat->format == VK_FORMAT_B8G8R8A8_UNORM ? nvrhi::Format::BGRA8_UNORM : nvrhi::Format::RGBA8_UNORM;
		m_PresentMode = presentMode;

		std::vector<VkImage> images;
		result = Utils::EnumerateVulkanArray(images, [device, this](uint32_t* count, VkImage* items)
		{
			return VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSwapchainImagesKHR(device, m_Swapchain, count, items);
		});
		if (result != VK_SUCCESS)
			return MakeError(ErrorCode::Gpu, "vkGetSwapchainImagesKHR failed with {}", VkResultToString(result));

		m_Images.reserve(images.size());
		for (size_t index = 0; index < images.size(); ++index)
		{
			SwapchainImage& image = m_Images.emplace_back();
			image.Handle = images[index];

			// NVRHI tracks the image from its first use (from an undefined layout) and returns it to the present layout at
			// the end of every command list that uses it.
			nvrhi::TextureDesc desc;
			desc.setWidth(m_Width)
				.setHeight(m_Height)
				.setFormat(m_Format)
				.setDimension(nvrhi::TextureDimension::Texture2D)
				.setIsRenderTarget(true)
				.setInitialState(nvrhi::ResourceStates::Present)
				.setKeepInitialState(true)
				.setDebugName(std::format("Swapchain image {}", index));
			ENGINE_TRY_ASSIGN(image.Texture, WithContext(m_Device->CreateHandleForNativeTexture(image.Handle, desc), "while wrapping the swapchain images"));
			ENGINE_TRY_ASSIGN(image.Framebuffer,
				WithContext(m_Device->CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(image.Texture)),
					"while creating the swapchain's framebuffers"));
			ENGINE_TRY_ASSIGN(image.PresentSemaphore,
				WithContext(Utils::CreateBinarySemaphore(device), "while creating the swapchain's present semaphores"));
		}
		return {};
	}

	Status Swapchain::Recreate(uint32_t width, uint32_t height)
	{
		// No image of the old swapchain, and no semaphore, may be in use by the GPU or the presentation engine.
		m_Device->WaitForIdle();
		ReleaseImages();
		m_HasAcquiredImage = false;
		m_IsRecreatePending = false;
		if (m_IsSurfaceLost)
		{
			// A swapchain cannot outlive its surface, and a new surface takes no old swapchain.
			DestroySwapchain();
			DestroySurface();
			m_IsSurfaceLost = false;
			ENGINE_TRY(WithContext(CreateSurface(), "while recreating the window's surface"));
		}
		ENGINE_TRY(WithContext(CreateSwapchain(width, height), "while recreating the swapchain"));
		if (m_Swapchain != VK_NULL_HANDLE)
		{
			++m_RecreationCount;
			ENGINE_CORE_TRACE("Swapchain recreated: {}x{}, {} images", m_Width, m_Height, m_Images.size());
		}
		return {};
	}

	void Swapchain::ReleaseImages()
	{
		if (m_Images.empty())
			return;
		for (SwapchainImage& image : m_Images)
		{
			image.Framebuffer = nullptr;
			image.Texture = nullptr;
		}
		// Retired submissions and the resource tracker still hold the framebuffers, and the framebuffers the wrapped images:
		// collecting now destroys them, and with them their image views, before the swapchain that owns the images goes.
		m_Device->RunGarbageCollection();

		const VkDevice device = m_Device->GetVulkanDevice();
		for (const SwapchainImage& image : m_Images)
		{
			if (image.PresentSemaphore != VK_NULL_HANDLE)
				VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(device, image.PresentSemaphore, nullptr);
		}
		m_Images.clear();
	}

	void Swapchain::DestroySwapchain()
	{
		if (m_Swapchain != VK_NULL_HANDLE)
		{
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySwapchainKHR(m_Device->GetVulkanDevice(), m_Swapchain, nullptr);
			m_Swapchain = VK_NULL_HANDLE;
		}
		m_Width = 0;
		m_Height = 0;
	}

	void Swapchain::DestroySurface()
	{
		if (m_Surface != VK_NULL_HANDLE)
		{
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySurfaceKHR(m_Device->GetVulkanInstance(), m_Surface, nullptr);
			m_Surface = VK_NULL_HANDLE;
		}
	}

	void Swapchain::EndProcess(std::string_view call, VkResult result)
	{
		if (result == VK_ERROR_DEVICE_LOST)
			m_Device->RaiseDeviceLost(call);
		FatalError(GetFatalErrorKind(result), std::format("{} failed with {}", call, VkResultToString(result)));
	}

	std::string_view SwapchainActionToString(SwapchainAction action)
	{
		switch (action)
		{
			case SwapchainAction::Continue:             return "Continue";
			case SwapchainAction::ContinueThenRecreate: return "ContinueThenRecreate";
			case SwapchainAction::Recreate:             return "Recreate";
			case SwapchainAction::RecreateSurface:      return "RecreateSurface";
			case SwapchainAction::Retry:                return "Retry";
			case SwapchainAction::DeviceLost:           return "DeviceLost";
			case SwapchainAction::OutOfMemory:          return "OutOfMemory";
			case SwapchainAction::Fail:                 return "Fail";
		}

		ENGINE_CORE_ASSERT(false, "Unknown SwapchainAction {}", std::to_underlying(action));
		return "Unknown";
	}

}
