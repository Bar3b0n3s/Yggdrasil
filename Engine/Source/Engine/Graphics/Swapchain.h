#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// The window's swapchain (Architecture §8.1): our own code, not NVRHI's. glfwCreateWindowSurface, a VkSwapchainKHR, and
// its images wrapped with GraphicsDevice::CreateHandleForNativeTexture. Format B8G8R8A8_UNORM, else R8G8B8A8_UNORM (UNORM
// holding sRGB-encoded values, §8.9). Present mode FIFO with VSync, else MAILBOX, else IMMEDIATE.
//
// Synchronization: one acquire semaphore per frame in flight and one render-complete (present) semaphore per swapchain
// image, attached through GraphicsDevice::QueueWaitForSemaphore and QueueSignalSemaphore to the frame's last
// executeCommandLists, then present.
//
// Every swapchain call uses the C entry points through the dispatcher (VULKAN_HPP_DEFAULT_DISPATCHER.vkAcquireNextImageKHR,
// vkQueuePresentKHR, ...) and switches on the returned VkResult; vulkan.hpp's enhanced-mode wrappers would throw
// vk::OutOfDateKHRError, which this state machine treats as an ordinary state (ClassifyAcquireResult,
// ClassifyPresentResult). Recreation on those results and on a framebuffer resize is the tested state machine of
// "Swapchain: resize, minimize and out-of-date recover", whose windowed child drives VK_ERROR_OUT_OF_DATE_KHR,
// VK_SUBOPTIMAL_KHR and VK_ERROR_SURFACE_LOST_KHR through the real acquire and present paths (InjectAcquireResultForTesting,
// InjectPresentResultForTesting); a 0x0 (minimized) framebuffer skips rendering, and the frame loop throttles itself while
// the window is minimized (§4.2).
//
// Acquisition is bounded but never fatal by itself: an acquire that times out means the presentation engine is holding
// its images back (a window that is hidden or occluded without being minimized: Wayland's FIFO, macOS occlusion), not
// that the GPU stopped, so the frame is skipped and the next frame tries again. A GPU that really stopped is caught by
// FramePacer's bounded waits on the graphics queue's timeline (FatalError(GpuHang), §8.1): a skipped frame records the
// device's last submission for its slot, and the next frame of that slot waits for it.

namespace Engine {

	class GraphicsDevice;
	class Window;

	// What the swapchain does after a VkResult (pure; unit-tested without a GPU).
	enum class SwapchainAction : uint8_t
	{
		Continue,             // VK_SUCCESS: go on
		ContinueThenRecreate, // VK_SUBOPTIMAL_KHR from acquire: the image is acquired and is rendered and presented; recreate after
		Recreate,             // VK_ERROR_OUT_OF_DATE_KHR (acquire or present), VK_SUBOPTIMAL_KHR from present: recreate the swapchain
		RecreateSurface,      // VK_ERROR_SURFACE_LOST_KHR: recreate the surface and the swapchain
		Retry,                // VK_TIMEOUT or VK_NOT_READY from the bounded acquire: no image this frame; the next frame tries again
		DeviceLost,           // VK_ERROR_DEVICE_LOST: GraphicsDevice::RaiseDeviceLost
		OutOfMemory,          // VK_ERROR_OUT_OF_HOST_MEMORY / VK_ERROR_OUT_OF_DEVICE_MEMORY: FatalError(OutOfMemory)
		Fail                  // anything else: FatalError(Gpu)
	};

	// The action for vkAcquireNextImageKHR's result.
	[[nodiscard]] SwapchainAction ClassifyAcquireResult(VkResult result);
	// The action for vkQueuePresentKHR's result (VK_TIMEOUT and VK_NOT_READY are not present results and give Fail).
	[[nodiscard]] SwapchainAction ClassifyPresentResult(VkResult result);

	struct SwapchainSpecification
	{
		// GraphicsSpecification::VSync.
		bool VSync = true;
		// GraphicsSpecification::FramesInFlight: the number of acquire semaphores. At least 1.
		uint32_t FramesInFlight = 2;
	};

	// What AcquireNextImage did.
	enum class SwapchainAcquireStatus : uint8_t
	{
		Acquired, // an image is acquired: render into GetCurrentFramebuffer, then QueueFrameSemaphores, submit, Present
		Skipped   // nothing to render this frame: the framebuffer is 0x0 (minimized), or the swapchain was just recreated
	};

	// One window's swapchain. Not copyable or movable; main thread only (§4.11).
	class Swapchain
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class Swapchain;
		};

		// Use Create.
		explicit Swapchain(ConstructionKey key);
		// GraphicsDevice::WaitForIdle, then the images' handles, the semaphores, the VkSwapchainKHR and the surface (§8.14
		// item 4: the swapchain goes before the NVRHI device).
		~Swapchain();

		Swapchain(const Swapchain&) = delete;
		Swapchain& operator=(const Swapchain&) = delete;

		// Creates the surface (glfwCreateWindowSurface on window.GetNativeHandle()) and the swapchain at the window's
		// framebuffer size. `device` and `window` are documented back-references and must outlive the swapchain; the
		// device must have been created with this window as GraphicsDeviceSpecification::PresentWindow. A 0x0 framebuffer
		// (or an iconified window) creates no VkSwapchainKHR yet (IsMinimized). Errors: InvalidState when the device has no
		// swapchain entry points (created without a PresentWindow); Unsupported when the surface supports neither format,
		// or the graphics queue family cannot present to it; Gpu for a failed Vulkan call or image wrapping.
		[[nodiscard]] static Result<Scope<Swapchain>> Create(GraphicsDevice& device, Window& window, const SwapchainSpecification& specification);

		// Acquires the next image for frame slot `frameSlot` (FramePacer::GetFrameSlot). First, when the window's
		// framebuffer size differs from the swapchain's (a resize) or a recreation is pending, recreates the swapchain
		// (GraphicsDevice::WaitForIdle first) and returns Skipped; a minimized window (Window::IsMinimized: iconified, or a
		// framebuffer with a zero dimension) returns Skipped without recreating, and releases the swapchain (WaitForIdle
		// first) until a later call sees it restored. However many resize events and results marked a recreation since the
		// last call, it happens once, at the framebuffer size of that moment.
		// Otherwise vkAcquireNextImageKHR with a timeout of GpuWaitSliceNanoseconds (FramePacer.h), acting on
		// ClassifyAcquireResult: Continue returns Acquired; ContinueThenRecreate returns Acquired and marks a recreation for
		// the next call; Recreate and RecreateSurface recreate and return Skipped; Retry returns Skipped (logged at Warn on
		// the first of consecutive timed-out acquires, so an occluded window does not flood the log); DeviceLost,
		// OutOfMemory and Fail end the process. Errors: Gpu when a recreation fails.
		[[nodiscard]] Result<SwapchainAcquireStatus> AcquireNextImage(uint32_t frameSlot);

		// Queues the current slot's acquire-semaphore wait and the current image's present-semaphore signal, which NVRHI
		// attaches to the next executeCommandLists: call it right before the frame's last submission, after an Acquired
		// AcquireNextImage.
		void QueueFrameSemaphores();

		// vkQueuePresentKHR of the current image, waiting on its present semaphore, acting on ClassifyPresentResult:
		// Recreate and RecreateSurface mark a recreation for the next AcquireNextImage; DeviceLost, OutOfMemory and Fail end
		// the process. Call it after the frame's last submission.
		void Present();

		// Marks a recreation for the next AcquireNextImage, for a caller that knows of one the size check cannot see. A
		// framebuffer size change needs no call: AcquireNextImage compares the size with the one the swapchain was created
		// for, so Application does not call it on WindowResizeEvent (several events of one resize would each mark one).
		void RequestRecreate();

		// Test hooks for the windowed child of "Swapchain: resize, minimize and out-of-date recover": they make the next real
		// call act on `result` through the same ClassifyAcquireResult or ClassifyPresentResult switch as a returned VkResult,
		// so the recreation paths run without a compositor that produces the result on demand. Production code never calls
		// them. Each replaces exactly one call; a second injection before that call replaces the first.
		//
		// InjectAcquireResultForTesting: VK_SUBOPTIMAL_KHR replaces the VK_SUCCESS of the next acquire that gets an image
		// (the image stays acquired, as with a real suboptimal result); VK_ERROR_OUT_OF_DATE_KHR and
		// VK_ERROR_SURFACE_LOST_KHR, which acquire no image, replace the next acquire itself (vkAcquireNextImageKHR is not
		// called). Any other result is a programmer error (asserted).
		void InjectAcquireResultForTesting(VkResult result);
		// InjectPresentResultForTesting: the next Present calls vkQueuePresentKHR and then acts on `result` instead of the
		// returned value (the image is presented, as with a real suboptimal or out-of-date present). `result` must be
		// VK_SUBOPTIMAL_KHR, VK_ERROR_OUT_OF_DATE_KHR or VK_ERROR_SURFACE_LOST_KHR (asserted).
		void InjectPresentResultForTesting(VkResult result);

		// The current image (valid after an Acquired AcquireNextImage until Present) and a framebuffer with it as the only
		// color attachment.
		[[nodiscard]] nvrhi::ITexture* GetCurrentTexture() const;
		[[nodiscard]] nvrhi::IFramebuffer* GetCurrentFramebuffer() const;
		// The swapchain format: BGRA8_UNORM or RGBA8_UNORM.
		[[nodiscard]] nvrhi::Format GetFormat() const;
		[[nodiscard]] uint32_t GetWidth() const;
		[[nodiscard]] uint32_t GetHeight() const;
		[[nodiscard]] uint32_t GetImageCount() const;
		// VK_PRESENT_MODE_FIFO_KHR, MAILBOX or IMMEDIATE.
		[[nodiscard]] VkPresentModeKHR GetPresentMode() const;
		// True while no VkSwapchainKHR exists because the window is minimized (iconified, or a framebuffer with a zero
		// dimension) or its surface reports a zero extent.
		[[nodiscard]] bool IsMinimized() const;
		// The number of recreations since Create (resize, out-of-date, suboptimal, surface loss); for tests.
		[[nodiscard]] uint32_t GetRecreationCount() const;
	private:
		// One swapchain image: the VkImage the swapchain owns, its NVRHI wrapper and framebuffer, and its present semaphore,
		// which the frame's last submission signals and vkQueuePresentKHR waits on.
		struct SwapchainImage
		{
			VkImage Handle = VK_NULL_HANDLE;
			nvrhi::TextureHandle Texture{};
			nvrhi::FramebufferHandle Framebuffer{};
			VkSemaphore PresentSemaphore = VK_NULL_HANDLE;
		};
	private:
		// glfwCreateWindowSurface and the presentation-support check of the graphics queue family.
		[[nodiscard]] Status CreateSurface();
		// A VkSwapchainKHR at the framebuffer size `width` x `height` (the surface's fixed extent wins), replacing and
		// destroying the current one, with its images wrapped. Creates none while the surface reports a zero extent.
		[[nodiscard]] Status CreateSwapchain(uint32_t width, uint32_t height);
		// GraphicsDevice::WaitForIdle, the images released, the surface recreated when it was lost, then CreateSwapchain.
		[[nodiscard]] Status Recreate(uint32_t width, uint32_t height);
		// Drops the images' handles, collects them (GraphicsDevice::RunGarbageCollection) and destroys the present
		// semaphores. The GPU must be idle.
		void ReleaseImages();
		void DestroySwapchain();
		void DestroySurface();
		// The fatal end of a swapchain call: GraphicsDevice::RaiseDeviceLost for VK_ERROR_DEVICE_LOST, otherwise
		// FatalError with GetFatalErrorKind(result).
		[[noreturn]] void EndProcess(std::string_view call, VkResult result);
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference: outlives the swapchain
		Window* m_Window = nullptr;         // documented back-reference: outlives the swapchain
		SwapchainSpecification m_Specification{};
		VkSurfaceKHR m_Surface = VK_NULL_HANDLE;
		VkSwapchainKHR m_Swapchain = VK_NULL_HANDLE;
		std::vector<SwapchainImage> m_Images;
		std::vector<VkSemaphore> m_AcquireSemaphores; // one per frame in flight
		nvrhi::Format m_Format = nvrhi::Format::UNKNOWN;
		VkPresentModeKHR m_PresentMode = VK_PRESENT_MODE_FIFO_KHR;
		// The swapchain's extent, and the framebuffer size it was created for (they differ when the surface fixes the extent).
		uint32_t m_Width = 0;
		uint32_t m_Height = 0;
		uint32_t m_RequestedWidth = 0;
		uint32_t m_RequestedHeight = 0;
		uint32_t m_CurrentImage = 0;
		uint32_t m_CurrentSlot = 0;
		uint32_t m_RecreationCount = 0;
		std::optional<VkResult> m_InjectedAcquireResult{};
		std::optional<VkResult> m_InjectedPresentResult{};
		bool m_HasAcquiredImage = false;
		bool m_IsRecreatePending = false;
		bool m_IsSurfaceLost = false;
		bool m_IsAcquireTimingOut = false; // the last acquire timed out, so the next timeout is not logged again
	};

	// The enumerator name ("Continue", "Recreate", ...).
	[[nodiscard]] std::string_view SwapchainActionToString(SwapchainAction action);

}
