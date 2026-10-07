#include "EnginePCH.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/VulkanDispatch.h"
#include "Engine/Platform/Environment.h"

// Order matters: glfw3.h declares its Vulkan functions (glfwGetRequiredInstanceExtensions,
// glfwGetPhysicalDevicePresentationSupport) only when the Vulkan headers come first.
#include <nvrhi/validation.h>
#include <nvrhi/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <format>
#include <optional>

// Device creation (Architecture §8.1) through the dispatcher's C entry points, which report failures as VkResult values.
// NVRHI's own Vulkan calls use vulkan.hpp's enhanced mode and throw vk::SystemError; this file is one of the allowlisted
// boundaries of §4.6 (item 1), where such an exception during device creation becomes a Gpu error.

namespace Engine {

	namespace {

		// What device selection learned about one physical device besides the plain DeviceCandidate.
		struct PhysicalDeviceInfo
		{
			VkPhysicalDevice Device = VK_NULL_HANDLE;
			DeviceCandidate Candidate{};
			uint32_t QueueFamily = 0;
			uint32_t MaxMemoryAllocationCount = 0;
			bool HasSwapchainExtension = false;
		};

	}

	namespace Utils {

		// The variable that overrides device selection when --gpu is not given (§8.1).
		constexpr std::string_view GpuEnvironmentVariable = "ENGINE_GPU";
		constexpr const char* ValidationLayerName = "VK_LAYER_KHRONOS_validation";
		// Dist never validates (GraphicsSpecification::Validation, §2.2).
		constexpr bool IsValidationBuild =
#if defined(ENGINE_DIST)
			false;
#else
			true;
#endif
		// String literals: the macros live in vulkan_beta.h, which the engine does not enable (Vendor/NVRHI/VENDOR.md).
		constexpr const char* PortabilitySubsetExtensionName = "VK_KHR_portability_subset";

		// A Gpu error for a failed Vulkan call; nothing for VK_SUCCESS.
		static Status CheckVulkan(VkResult result, std::string_view call)
		{
			if (result == VK_SUCCESS)
				return {};
			return MakeError(ErrorCode::Gpu, "{} failed: {}", call, VkResultToString(result));
		}

		// "1.4.341" for a VK_MAKE_API_VERSION value.
		static std::string ApiVersionToString(uint32_t version)
		{
			return std::format("{}.{}.{}", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), VK_API_VERSION_PATCH(version));
		}

		// The major.minor part of `version`, so versions compare without their patch.
		static uint32_t GetMajorMinor(uint32_t version)
		{
			return VK_MAKE_API_VERSION(0, VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version), 0);
		}

		static bool ContainsName(const std::vector<std::string>& names, std::string_view name)
		{
			return std::ranges::find(names, name) != names.end();
		}

		static void AddUnique(std::vector<std::string>& names, std::string_view name)
		{
			if (!ContainsName(names, name))
				names.emplace_back(name);
		}

		// The instance extensions of the loader and its implicit layers, or of the layer `layerName`.
		static Result<std::vector<std::string>> EnumerateInstanceExtensions(const char* layerName)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			std::vector<VkExtensionProperties> properties;
			VkResult result = VK_INCOMPLETE;
			while (result == VK_INCOMPLETE)
			{
				uint32_t count = 0;
				ENGINE_TRY(CheckVulkan(dispatcher.vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr),
					"vkEnumerateInstanceExtensionProperties"));
				properties.resize(count);
				result = dispatcher.vkEnumerateInstanceExtensionProperties(layerName, &count, properties.data());
				properties.resize(count);
			}
			ENGINE_TRY(CheckVulkan(result, "vkEnumerateInstanceExtensionProperties"));

			std::vector<std::string> names;
			names.reserve(properties.size());
			for (const VkExtensionProperties& extension : properties)
				names.emplace_back(extension.extensionName);
			return names;
		}

		static Result<bool> IsInstanceLayerAvailable(const char* layerName)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			std::vector<VkLayerProperties> layers;
			VkResult result = VK_INCOMPLETE;
			while (result == VK_INCOMPLETE)
			{
				uint32_t count = 0;
				ENGINE_TRY(CheckVulkan(dispatcher.vkEnumerateInstanceLayerProperties(&count, nullptr), "vkEnumerateInstanceLayerProperties"));
				layers.resize(count);
				result = dispatcher.vkEnumerateInstanceLayerProperties(&count, layers.data());
				layers.resize(count);
			}
			ENGINE_TRY(CheckVulkan(result, "vkEnumerateInstanceLayerProperties"));
			return std::ranges::any_of(layers, [layerName](const VkLayerProperties& layer)
			{
				return std::string_view(layer.layerName) == layerName;
			});
		}

		static Result<std::vector<std::string>> EnumerateDeviceExtensions(VkPhysicalDevice device)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			std::vector<VkExtensionProperties> properties;
			VkResult result = VK_INCOMPLETE;
			while (result == VK_INCOMPLETE)
			{
				uint32_t count = 0;
				ENGINE_TRY(CheckVulkan(dispatcher.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr),
					"vkEnumerateDeviceExtensionProperties"));
				properties.resize(count);
				result = dispatcher.vkEnumerateDeviceExtensionProperties(device, nullptr, &count, properties.data());
				properties.resize(count);
			}
			ENGINE_TRY(CheckVulkan(result, "vkEnumerateDeviceExtensionProperties"));

			std::vector<std::string> names;
			names.reserve(properties.size());
			for (const VkExtensionProperties& extension : properties)
				names.emplace_back(extension.extensionName);
			return names;
		}

		static Result<std::vector<VkPhysicalDevice>> EnumeratePhysicalDevices(VkInstance instance)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			std::vector<VkPhysicalDevice> devices;
			VkResult result = VK_INCOMPLETE;
			while (result == VK_INCOMPLETE)
			{
				uint32_t count = 0;
				ENGINE_TRY(CheckVulkan(dispatcher.vkEnumeratePhysicalDevices(instance, &count, nullptr), "vkEnumeratePhysicalDevices"));
				devices.resize(count);
				result = dispatcher.vkEnumeratePhysicalDevices(instance, &count, devices.data());
				devices.resize(count);
			}
			ENGINE_TRY(CheckVulkan(result, "vkEnumeratePhysicalDevices"));
			return devices;
		}

		static GpuDeviceType ToGpuDeviceType(VkPhysicalDeviceType type)
		{
			switch (type)
			{
				case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return GpuDeviceType::Discrete;
				case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return GpuDeviceType::Integrated;
				case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return GpuDeviceType::Virtual;
				case VK_PHYSICAL_DEVICE_TYPE_CPU:            return GpuDeviceType::Cpu;
				default:                                     return GpuDeviceType::Other;
			}
		}

		static bool HasOptimalTilingFeature(VkPhysicalDevice device, nvrhi::Format format, VkFormatFeatureFlags feature)
		{
			VkFormatProperties properties{};
			VULKAN_HPP_DEFAULT_DISPATCHER.vkGetPhysicalDeviceFormatProperties(device, nvrhi::vulkan::convertFormat(format), &properties);
			return (properties.optimalTilingFeatures & feature) == feature;
		}

		// Whether a device with host image copy can copy into, and transition to, SHADER_READ_ONLY_OPTIMAL: the layout
		// HostImageUpload leaves its images in, which NVRHI's ShaderResource state means.
		static bool CanHostCopyToShaderReadOnly(VkPhysicalDevice device)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			VkPhysicalDeviceHostImageCopyProperties copyProperties{};
			copyProperties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES;
			VkPhysicalDeviceProperties2 properties{};
			properties.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
			properties.pNext = &copyProperties;
			dispatcher.vkGetPhysicalDeviceProperties2(device, &properties);
			std::vector<VkImageLayout> destinationLayouts(copyProperties.copyDstLayoutCount);
			copyProperties.copySrcLayoutCount = 0;
			copyProperties.pCopySrcLayouts = nullptr;
			copyProperties.pCopyDstLayouts = destinationLayouts.data();
			dispatcher.vkGetPhysicalDeviceProperties2(device, &properties);
			destinationLayouts.resize(copyProperties.copyDstLayoutCount);
			return std::ranges::find(destinationLayouts, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) != destinationLayouts.end();
		}

		// Fills the DeviceCandidate of one physical device (§8.1) and picks its graphics queue family. `instanceApiVersion`
		// limits the feature structures that may be queried: a Vulkan 1.4 structure only with a 1.4 instance.
		static Result<PhysicalDeviceInfo> DescribePhysicalDevice(VkInstance instance, VkPhysicalDevice device, uint32_t index,
			uint32_t instanceApiVersion, bool presenting)
		{
			const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
			PhysicalDeviceInfo info;
			info.Device = device;
			DeviceCandidate& candidate = info.Candidate;
			candidate.Index = index;

			VkPhysicalDeviceProperties properties{};
			dispatcher.vkGetPhysicalDeviceProperties(device, &properties);
			candidate.Name = properties.deviceName;
			candidate.Type = ToGpuDeviceType(properties.deviceType);
			candidate.ApiVersion = properties.apiVersion;
			candidate.VendorID = properties.vendorID;
			candidate.DeviceID = properties.deviceID;
			candidate.DriverVersion = properties.driverVersion;
			info.MaxMemoryAllocationCount = properties.limits.maxMemoryAllocationCount;

			const uint32_t deviceApiVersion = GetMajorMinor(properties.apiVersion);
			const uint32_t usableApiVersion = std::min(deviceApiVersion, GetMajorMinor(instanceApiVersion));
			if (usableApiVersion >= VK_API_VERSION_1_2)
			{
				VkPhysicalDeviceDriverProperties driver{};
				driver.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
				VkPhysicalDeviceProperties2 properties2{};
				properties2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
				properties2.pNext = &driver;
				dispatcher.vkGetPhysicalDeviceProperties2(device, &properties2);
				candidate.DriverID = driver.driverID;
			}

			ENGINE_TRY_ASSIGN(const std::vector<std::string> extensions, EnumerateDeviceExtensions(device));
			info.HasSwapchainExtension = ContainsName(extensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
			const bool hasDeviceFaultExtension = ContainsName(extensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
			candidate.PortabilitySubset = ContainsName(extensions, PortabilitySubsetExtensionName);

			// The features. The 1.2 and 1.3 structures need a 1.3 device (anything older is rejected by its version).
			VkPhysicalDeviceFeatures2 features{};
			features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
			VkPhysicalDeviceVulkan12Features features12{};
			features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
			VkPhysicalDeviceVulkan13Features features13{};
			features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
			VkPhysicalDeviceVulkan14Features features14{};
			features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
			VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
			faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
			const bool queryVulkan14 = usableApiVersion >= VK_API_VERSION_1_4;
			if (usableApiVersion >= VK_API_VERSION_1_3)
			{
				features.pNext = &features12;
				features12.pNext = &features13;
				void** tail = &features13.pNext;
				if (queryVulkan14)
				{
					*tail = &features14;
					tail = &features14.pNext;
				}
				if (hasDeviceFaultExtension)
					*tail = &faultFeatures;
				dispatcher.vkGetPhysicalDeviceFeatures2(device, &features);
			}
			else
			{
				dispatcher.vkGetPhysicalDeviceFeatures(device, &features.features);
			}

			candidate.DynamicRendering = features13.dynamicRendering == VK_TRUE;
			candidate.Synchronization2 = features13.synchronization2 == VK_TRUE;
			candidate.TimelineSemaphore = features12.timelineSemaphore == VK_TRUE;
			candidate.SamplerAnisotropy = features.features.samplerAnisotropy == VK_TRUE;
			candidate.ImageCubeArray = features.features.imageCubeArray == VK_TRUE;
			candidate.ShaderStorageImageExtendedFormats = features.features.shaderStorageImageExtendedFormats == VK_TRUE;
			candidate.DepthClamp = features.features.depthClamp == VK_TRUE;
			candidate.FillModeNonSolid = features.features.fillModeNonSolid == VK_TRUE;
			candidate.DeviceFault = hasDeviceFaultExtension && faultFeatures.deviceFault == VK_TRUE;
			candidate.HostImageCopy = queryVulkan14 && features14.hostImageCopy == VK_TRUE && CanHostCopyToShaderReadOnly(device);

			for (size_t format = 0; format < RequiredStorageFormats.size(); ++format)
			{
				candidate.StorageFormatSupported[format] =
					HasOptimalTilingFeature(device, RequiredStorageFormats[format], VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
			}
			candidate.B10G11R11StorageSupported = HasOptimalTilingFeature(device, nvrhi::Format::R11G11B10_FLOAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

			// The first family with graphics and compute, preferring one that can present when the process presents.
			uint32_t familyCount = 0;
			dispatcher.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
			std::vector<VkQueueFamilyProperties> families(familyCount);
			dispatcher.vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
			families.resize(familyCount);
			std::optional<uint32_t> graphicsFamily;
			std::optional<uint32_t> presentFamily;
			for (uint32_t family = 0; family < familyCount; ++family)
			{
				constexpr VkQueueFlags RequiredFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
				if ((families[family].queueFlags & RequiredFlags) != RequiredFlags || families[family].queueCount == 0)
					continue;
				if (!graphicsFamily.has_value())
					graphicsFamily = family;
				if (presenting && !presentFamily.has_value() && glfwGetPhysicalDevicePresentationSupport(instance, device, family) == GLFW_TRUE)
					presentFamily = family;
			}
			candidate.HasGraphicsQueue = graphicsFamily.has_value();
			candidate.HasPresentSupport = presentFamily.has_value() && info.HasSwapchainExtension;
			info.QueueFamily = presentFamily.value_or(graphicsFamily.value_or(0));
			return info;
		}

		// The debug messenger's callback: every message goes to the device's GpuDiagnostics, which logs and counts it
		// (§8.1), except the loader's notes about the installation, which it logs without counting
		// (GpuDiagnostics::ReportDebugUtilsMessage). Never aborts the call that triggered it.
		static VKAPI_ATTR VkBool32 VKAPI_CALL ReceiveDebugMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
			VkDebugUtilsMessageTypeFlagsEXT types, const VkDebugUtilsMessengerCallbackDataEXT* data, void* userData)
		{
			GpuDiagnostics* diagnostics = static_cast<GpuDiagnostics*>(userData);
			if (diagnostics == nullptr)
				return VK_FALSE;
			const bool hasText = data != nullptr && data->pMessage != nullptr;
			const bool hasId = data != nullptr && data->pMessageIdName != nullptr;
			const std::string_view text = hasText ? std::string_view(data->pMessage) : std::string_view();
			const std::string_view id = hasId ? std::string_view(data->pMessageIdName) : std::string_view();
			diagnostics->ReportDebugUtilsMessage(severity, types, id, text);
			return VK_FALSE;
		}

		// "'SceneColor' (1920x1080 RGBA16_FLOAT)", how creation errors name a texture.
		static std::string DescribeTexture(const nvrhi::TextureDesc& desc)
		{
			std::string size = std::format("{}x{}", desc.width, desc.height);
			if (desc.depth > 1)
				size += std::format("x{}", desc.depth);
			if (desc.arraySize > 1)
				size += std::format(", {} slices", desc.arraySize);
			if (desc.mipLevels > 1)
				size += std::format(", {} mips", desc.mipLevels);
			return std::format("'{}' ({} {})", desc.debugName.empty() ? std::string("unnamed") : desc.debugName, size,
				nvrhi::getFormatInfo(desc.format).name);
		}

		static std::string_view DebugNameOrUnnamed(const std::string& name)
		{
			return name.empty() ? std::string_view("unnamed") : std::string_view(name);
		}

		// The text of a fixed-size, NUL-terminated Vulkan string member, bounded by its array even without a terminator.
		template<size_t Size>
		static std::string_view FixedStringView(const char (&text)[Size])
		{
			return std::string_view(text, static_cast<size_t>(std::find(text, text + Size, '\0') - text));
		}

		// The debug name of the first shader a pipeline description names, for creation errors.
		static std::string_view GetShaderName(nvrhi::IShader* shader)
		{
			return shader != nullptr ? DebugNameOrUnnamed(shader->getDesc().debugName) : std::string_view("none");
		}

	}

	GraphicsDevice::GraphicsDevice(ConstructionKey /*key*/, const GraphicsSpecification& specification)
		: m_Specification(specification), m_Diagnostics(specification.InjectFault)
	{
	}

	GraphicsDevice::~GraphicsDevice()
	{
		Shutdown();
	}

	GpuMessageCounts GraphicsDevice::Destroy(Scope<GraphicsDevice> device)
	{
		if (device == nullptr)
			return {};
		device->Shutdown();
		const GpuMessageCounts counts = device->m_Diagnostics.GetCounts();
		device.reset();
		return counts;
	}

	void GraphicsDevice::Shutdown()
	{
		if (m_IsShutDown)
			return;
		m_IsShutDown = true;
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;

		// 1. Nothing may still run on the GPU, and the retired command lists drop their references. The C entry point
		// reports failures as values (§4.6 item 2): nothing may throw out of a destructor.
		if (m_NvrhiDevice)
		{
			const VkResult idle = dispatcher.vkDeviceWaitIdle(m_Device);
			if (idle != VK_SUCCESS)
				ENGINE_CORE_ERROR("vkDeviceWaitIdle failed at GPU device shutdown: {}", VkResultToString(idle));
			m_NvrhiDevice->runGarbageCollection();
		}

		// 2. The host-copied images, which NVRHI does not own, then what only the tracker still holds. A leak counts as a
		// GPU error, so the checks that read the counts after the teardown (Destroy) see it.
		m_HostImageUpload.reset();
		m_ResourceTracker.Sweep();
		if (m_ResourceTracker.GetTotalLiveCount() > 0)
		{
			m_Diagnostics.ReportMessage(GpuMessageSeverity::Error, "GPU resource tracker",
				std::format("GPU objects are still alive when the device is destroyed: {}", m_ResourceTracker.DescribeLiveCounts()));
		}
		m_ResourceTracker.ReleaseAll();

		// 3. §8.14 item 4: NVRHI device, VkDevice, debug messenger, VkInstance.
		m_NvrhiDevice = nullptr;
		m_VulkanNvrhiDevice = nullptr;
		if (m_Device != VK_NULL_HANDLE)
			dispatcher.vkDestroyDevice(m_Device, nullptr);
		if (m_DebugMessenger != VK_NULL_HANDLE)
			dispatcher.vkDestroyDebugUtilsMessengerEXT(m_Instance, m_DebugMessenger, nullptr);
		if (m_Instance != VK_NULL_HANDLE)
			dispatcher.vkDestroyInstance(m_Instance, nullptr);

		// 4. The default dispatcher keeps only the loader's global commands, so no pointer into a destroyed instance or
		// device survives (the next device initializes it again).
		if (m_IsRegistered)
		{
			const auto getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(VulkanDispatch::GetInstanceProcAddr());
			VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic();
			if (getInstanceProcAddr != nullptr)
				VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
			VulkanDispatch::UnregisterDevice();
		}
	}

	Result<Scope<GraphicsDevice>> GraphicsDevice::Create(const GraphicsDeviceSpecification& specification)
	{
		const GraphicsSpecification& graphics = specification.Graphics;
		if (graphics.FramesInFlight == 0)
			return MakeError(ErrorCode::InvalidArgument, "FramesInFlight must be at least 1");
		if constexpr (!GpuTestHooksEnabled)
		{
			if (graphics.InjectFault != GpuFault::None)
				return MakeError(ErrorCode::InvalidArgument, "--gpu-inject-fault={} does not exist in this build", GpuFaultToString(graphics.InjectFault));
		}
		if (!VulkanDispatch::IsInitialized())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the process has no Vulkan loader")
					.WithHint("the process context loads it when the application renders (VulkanLoaderPolicy)"));
		}
		if (VulkanDispatch::HasDevice())
			return MakeError(ErrorCode::InvalidState, "a GraphicsDevice already exists; a process has at most one at a time");

		Scope<GraphicsDevice> device = CreateScope<GraphicsDevice>(ConstructionKey(), graphics);
		VulkanDispatch::RegisterDevice();
		device->m_IsRegistered = true;
		ENGINE_TRY(device->CreateInstance(specification));
		ENGINE_TRY(device->CreateLogicalDevice(specification));
		ENGINE_TRY(device->CreateNvrhiDevice());
		device->m_HostImageUpload = CreateScope<HostImageUpload>(*device);
		device->LogDeviceInfo();
		return device;
	}

	Status GraphicsDevice::CreateInstance(const GraphicsDeviceSpecification& specification)
	{
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;

		// The API version: 1.4 when the loader has it and the cap allows it, otherwise 1.3 (§8.1).
		const uint32_t loaderVersion = VulkanDispatch::GetLoaderApiVersion();
		if (Utils::GetMajorMinor(loaderVersion) < VK_API_VERSION_1_3)
		{
			return std::unexpected(Error(ErrorCode::Unsupported,
				std::format("the Vulkan loader supports API {}, the engine needs 1.3", Utils::ApiVersionToString(loaderVersion)))
					.WithHint("install a GPU driver with Vulkan 1.3 support"));
		}
		const bool useVulkan14 = m_Specification.MaxApiVersion == VulkanApiVersion::Vulkan14 && Utils::GetMajorMinor(loaderVersion) >= VK_API_VERSION_1_4;
		const uint32_t apiVersion = useVulkan14 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;
		m_Info.ApiVersion = useVulkan14 ? VulkanApiVersion::Vulkan14 : VulkanApiVersion::Vulkan13;

		ENGINE_TRY_ASSIGN(const std::vector<std::string> available, Utils::EnumerateInstanceExtensions(nullptr));
		std::vector<std::string> availableWithLayer = available;
		std::vector<const char*> layers;
		const bool validation = m_Specification.Validation && Utils::IsValidationBuild;
		const bool synchronizationValidation = validation && m_Specification.SynchronizationValidation;
		bool useLayerSettings = false;
		if (validation)
		{
			ENGINE_TRY_ASSIGN(const bool hasLayer, Utils::IsInstanceLayerAvailable(Utils::ValidationLayerName));
			if (!hasLayer)
			{
				return std::unexpected(
					Error(ErrorCode::Unsupported, std::format("the Vulkan validation layer {} is not installed", Utils::ValidationLayerName))
						.WithHint("install the Vulkan SDK, or run without GPU validation"));
			}
			layers.push_back(Utils::ValidationLayerName);
			ENGINE_TRY_ASSIGN(const std::vector<std::string> layerExtensions, Utils::EnumerateInstanceExtensions(Utils::ValidationLayerName));
			for (const std::string& extension : layerExtensions)
				Utils::AddUnique(availableWithLayer, extension);

			Utils::AddUnique(m_InstanceExtensions, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			if (synchronizationValidation)
			{
				// The layer's settings extension, or the older validation-features extension.
				if (Utils::ContainsName(availableWithLayer, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME))
				{
					Utils::AddUnique(m_InstanceExtensions, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
					useLayerSettings = true;
				}
				else if (Utils::ContainsName(availableWithLayer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME))
				{
					Utils::AddUnique(m_InstanceExtensions, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				}
				else
				{
					return MakeError(ErrorCode::Unsupported,
						"the validation layer offers neither {} nor {}, which synchronization validation needs",
						VK_EXT_LAYER_SETTINGS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
				}
			}
		}

		VkInstanceCreateFlags flags = 0;
#if defined(ENGINE_PLATFORM_MACOS)
		// MoltenVK and KosmicKrisp are portability implementations, enumerated only with this flag (§8.1).
		if (Utils::ContainsName(available, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
		{
			Utils::AddUnique(m_InstanceExtensions, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
			flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
		}
#endif

		if (specification.PresentWindow != nullptr)
		{
			uint32_t count = 0;
			const char** surfaceExtensions = glfwGetRequiredInstanceExtensions(&count);
			if (surfaceExtensions == nullptr)
				return MakeError(ErrorCode::Unsupported, "GLFW cannot create Vulkan surfaces on this platform");
			for (uint32_t index = 0; index < count; ++index)
				Utils::AddUnique(m_InstanceExtensions, surfaceExtensions[index]);
		}

		for (const std::string& extension : m_InstanceExtensions)
		{
			if (!Utils::ContainsName(availableWithLayer, extension))
				return MakeError(ErrorCode::Unsupported, "the Vulkan instance extension {} is not available", extension);
		}

		// Validation messages during vkCreateInstance and vkDestroyInstance reach the diagnostics too.
		VkDebugUtilsMessengerCreateInfoEXT messengerInfo{};
		messengerInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		messengerInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		messengerInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
			| VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		messengerInfo.pfnUserCallback = &Utils::ReceiveDebugMessage;
		messengerInfo.pUserData = &m_Diagnostics;
		const VkBool32 enableSynchronizationValidation = VK_TRUE;
		VkLayerSettingEXT synchronizationSetting{};
		synchronizationSetting.pLayerName = Utils::ValidationLayerName;
		synchronizationSetting.pSettingName = "validate_sync";
		synchronizationSetting.type = VK_LAYER_SETTING_TYPE_BOOL32_EXT;
		synchronizationSetting.valueCount = 1;
		synchronizationSetting.pValues = &enableSynchronizationValidation;
		VkLayerSettingsCreateInfoEXT layerSettings{};
		layerSettings.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
		layerSettings.settingCount = 1;
		layerSettings.pSettings = &synchronizationSetting;
		const VkValidationFeatureEnableEXT enabledValidationFeature = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
		VkValidationFeaturesEXT validationFeatures{};
		validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
		validationFeatures.enabledValidationFeatureCount = 1;
		validationFeatures.pEnabledValidationFeatures = &enabledValidationFeature;
		const void* next = nullptr;
		if (validation)
		{
			next = &messengerInfo;
			if (synchronizationValidation && useLayerSettings)
			{
				layerSettings.pNext = next;
				next = &layerSettings;
			}
			else if (synchronizationValidation)
			{
				validationFeatures.pNext = next;
				next = &validationFeatures;
			}
		}

		VkApplicationInfo applicationInfo{};
		applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		applicationInfo.pApplicationName = specification.ApplicationName.c_str();
		applicationInfo.applicationVersion = 1;
		applicationInfo.pEngineName = "Engine";
		applicationInfo.engineVersion = 1;
		applicationInfo.apiVersion = apiVersion;
		std::vector<const char*> extensionNames;
		extensionNames.reserve(m_InstanceExtensions.size());
		for (const std::string& extension : m_InstanceExtensions)
			extensionNames.push_back(extension.c_str());
		VkInstanceCreateInfo instanceInfo{};
		instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceInfo.pNext = next;
		instanceInfo.flags = flags;
		instanceInfo.pApplicationInfo = &applicationInfo;
		instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
		instanceInfo.ppEnabledLayerNames = layers.data();
		instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		instanceInfo.ppEnabledExtensionNames = extensionNames.data();
		const VkResult created = dispatcher.vkCreateInstance(&instanceInfo, nullptr, &m_Instance);
		if (created != VK_SUCCESS)
		{
			m_Instance = VK_NULL_HANDLE;
			return MakeError(ErrorCode::Gpu, "vkCreateInstance (Vulkan {}) failed: {}", VulkanApiVersionToString(m_Info.ApiVersion),
				VkResultToString(created));
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Instance(m_Instance));

		if (validation)
		{
			ENGINE_TRY(Utils::CheckVulkan(dispatcher.vkCreateDebugUtilsMessengerEXT(m_Instance, &messengerInfo, nullptr, &m_DebugMessenger),
				"vkCreateDebugUtilsMessengerEXT"));
		}
		m_Info.Validation = validation;
		m_Info.SynchronizationValidation = synchronizationValidation;
		return {};
	}

	Status GraphicsDevice::CreateLogicalDevice(const GraphicsDeviceSpecification& specification)
	{
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;
		const bool presenting = specification.PresentWindow != nullptr;
		const uint32_t instanceApiVersion = m_Info.ApiVersion == VulkanApiVersion::Vulkan14 ? VK_API_VERSION_1_4 : VK_API_VERSION_1_3;

		// Selection over every physical device (§8.1).
		ENGINE_TRY_ASSIGN(const std::vector<VkPhysicalDevice> physicalDevices, Utils::EnumeratePhysicalDevices(m_Instance));
		std::vector<PhysicalDeviceInfo> devices;
		std::vector<DeviceCandidate> candidates;
		devices.reserve(physicalDevices.size());
		candidates.reserve(physicalDevices.size());
		for (uint32_t index = 0; index < physicalDevices.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(PhysicalDeviceInfo info,
				Utils::DescribePhysicalDevice(m_Instance, physicalDevices[index], index, instanceApiVersion, presenting));
			candidates.push_back(info.Candidate);
			devices.push_back(std::move(info));
		}

		DeviceSelectionRequest request{ .RequirePresent = presenting, .Override = m_Specification.GpuOverride };
		if (request.Override.empty())
			request.Override = ReadEnvironmentVariable(Utils::GpuEnvironmentVariable).value_or(std::string());
		ENGINE_TRY_ASSIGN(const DeviceSelection selection, SelectDevice(candidates, request));
		const PhysicalDeviceInfo& chosen = devices[selection.Index];
		const DeviceCandidate& candidate = chosen.Candidate;
		m_PhysicalDevice = chosen.Device;

		// The device uses min(instance, device) API version; 1.4 capabilities only at 1.4.
		const bool deviceVulkan14 = m_Info.ApiVersion == VulkanApiVersion::Vulkan14 && Utils::GetMajorMinor(candidate.ApiVersion) >= VK_API_VERSION_1_4;
		m_Info.ApiVersion = deviceVulkan14 ? VulkanApiVersion::Vulkan14 : VulkanApiVersion::Vulkan13;
		m_Info.DeviceName = candidate.Name;
		m_Info.Type = candidate.Type;
		m_Info.VendorID = candidate.VendorID;
		m_Info.DeviceID = candidate.DeviceID;
		m_Info.DriverVersion = candidate.DriverVersion;
		m_Info.DriverID = candidate.DriverID;
		m_Info.DeviceClass = GetDeviceClass(candidate.VendorID, candidate.DriverVersion, candidate.DriverID);
		m_Info.HostImageCopy = deviceVulkan14 && candidate.HostImageCopy;
		m_Info.DeviceFault = candidate.DeviceFault;
		m_Info.DepthClamp = candidate.DepthClamp;
		m_Info.FillModeNonSolid = candidate.FillModeNonSolid;
		m_Info.PortabilitySubset = candidate.PortabilitySubset;
		m_Info.BloomFormat = selection.BloomFormat;
		m_Info.MaxMemoryAllocationCount = chosen.MaxMemoryAllocationCount;
		m_Info.GraphicsQueueFamily = chosen.QueueFamily;

		if (presenting)
			Utils::AddUnique(m_DeviceExtensions, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
		if (m_Info.DeviceFault)
			Utils::AddUnique(m_DeviceExtensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
		if (m_Info.PortabilitySubset)
			Utils::AddUnique(m_DeviceExtensions, Utils::PortabilitySubsetExtensionName);

		// The 1.3 feature set NVRHI needs, the required features of §8.1 and the optional ones the device has.
		VkPhysicalDeviceFaultFeaturesEXT faultFeatures{};
		faultFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT;
		faultFeatures.deviceFault = VK_TRUE;
		VkPhysicalDeviceVulkan14Features features14{};
		features14.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES;
		features14.hostImageCopy = VK_TRUE;
		VkPhysicalDeviceVulkan13Features features13{};
		features13.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
		features13.synchronization2 = VK_TRUE;
		features13.dynamicRendering = VK_TRUE;
		VkPhysicalDeviceVulkan12Features features12{};
		features12.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
		features12.pNext = &features13;
		features12.timelineSemaphore = VK_TRUE;
		VkPhysicalDeviceFeatures2 features{};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &features12;
		features.features.samplerAnisotropy = VK_TRUE;
		features.features.imageCubeArray = VK_TRUE;
		features.features.shaderStorageImageExtendedFormats = VK_TRUE;
		features.features.depthClamp = m_Info.DepthClamp ? VK_TRUE : VK_FALSE;
		features.features.fillModeNonSolid = m_Info.FillModeNonSolid ? VK_TRUE : VK_FALSE;
		void** tail = &features13.pNext;
		if (m_Info.HostImageCopy)
		{
			*tail = &features14;
			tail = &features14.pNext;
		}
		if (m_Info.DeviceFault)
			*tail = &faultFeatures;

		const float queuePriority = 1.0f;
		VkDeviceQueueCreateInfo queueInfo{};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = chosen.QueueFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &queuePriority;
		std::vector<const char*> extensionNames;
		extensionNames.reserve(m_DeviceExtensions.size());
		for (const std::string& extension : m_DeviceExtensions)
			extensionNames.push_back(extension.c_str());
		VkDeviceCreateInfo deviceInfo{};
		deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceInfo.pNext = &features;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensionNames.size());
		deviceInfo.ppEnabledExtensionNames = extensionNames.data();
		const VkResult created = dispatcher.vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device);
		if (created != VK_SUCCESS)
		{
			m_Device = VK_NULL_HANDLE;
			return MakeError(ErrorCode::Gpu, "vkCreateDevice on '{}' failed: {}", m_Info.DeviceName, VkResultToString(created));
		}
		VULKAN_HPP_DEFAULT_DISPATCHER.init(vk::Device(m_Device));
		dispatcher.vkGetDeviceQueue(m_Device, chosen.QueueFamily, 0, &m_GraphicsQueue);

		if (m_Info.BloomFormat != nvrhi::Format::R11G11B10_FLOAT)
			ENGINE_CORE_INFO("Bloom renders to RGBA16_FLOAT: '{}' cannot store B10G11R11_UFLOAT images", m_Info.DeviceName);
		return {};
	}

	Status GraphicsDevice::CreateNvrhiDevice()
	{
		std::vector<const char*> instanceExtensions;
		for (const std::string& extension : m_InstanceExtensions)
			instanceExtensions.push_back(extension.c_str());
		std::vector<const char*> deviceExtensions;
		for (const std::string& extension : m_DeviceExtensions)
			deviceExtensions.push_back(extension.c_str());

		nvrhi::vulkan::DeviceDesc desc;
		desc.errorCB = &m_Diagnostics;
		desc.instance = m_Instance;
		desc.physicalDevice = m_PhysicalDevice;
		desc.device = m_Device;
		desc.graphicsQueue = m_GraphicsQueue;
		desc.graphicsQueueIndex = static_cast<int>(m_Info.GraphicsQueueFamily);
		desc.transferQueue = VK_NULL_HANDLE;
		desc.computeQueue = VK_NULL_HANDLE;
		desc.instanceExtensions = instanceExtensions.data();
		desc.numInstanceExtensions = instanceExtensions.size();
		desc.deviceExtensions = deviceExtensions.data();
		desc.numDeviceExtensions = deviceExtensions.size();
		// NVRHI's timer-query pool must hold every query the frame profilers create: a GpuProfiler times up to
		// MaxScopesPerFrame scopes per frame slot, and the application's profiler and a capture's may coexist.
		desc.maxTimerQueries = std::max<uint32_t>(desc.maxTimerQueries, GpuProfiler::MaxScopesPerFrame * m_Specification.FramesInFlight * 2);

		// NVRHI calls vulkan.hpp's throwing wrappers while it creates its queue objects (§4.6 item 1).
		try
		{
			m_VulkanNvrhiDevice = nvrhi::vulkan::createDevice(desc);
		}
		catch (const vk::SystemError& error)
		{
			return MakeError(ErrorCode::Gpu, "cannot create the NVRHI device on '{}': {}", m_Info.DeviceName, error.what());
		}
		if (!m_VulkanNvrhiDevice)
			return MakeError(ErrorCode::Gpu, "cannot create the NVRHI device on '{}'", m_Info.DeviceName);

		m_NvrhiDevice = m_Info.Validation ? nvrhi::validation::createValidationLayer(m_VulkanNvrhiDevice) : m_VulkanNvrhiDevice;
		if (!m_NvrhiDevice)
			return MakeError(ErrorCode::Gpu, "cannot create NVRHI's validation layer on '{}'", m_Info.DeviceName);

		auto* vulkanDevice = static_cast<nvrhi::vulkan::IDevice*>(m_VulkanNvrhiDevice.Get());
		m_GraphicsQueueTimelineSemaphore = vulkanDevice->getQueueSemaphore(nvrhi::CommandQueue::Graphics);
		return {};
	}

	void GraphicsDevice::LogDeviceInfo() const
	{
		std::string paths;
		const auto addPath = [&paths](bool enabled, std::string_view name)
		{
			if (!enabled)
				return;
			if (!paths.empty())
				paths += ", ";
			paths += name;
		};
		addPath(m_Info.HostImageCopy, "host image copy");
		addPath(m_Info.DeviceFault, "device fault");
		addPath(m_Info.DepthClamp, "depth clamp");
		addPath(m_Info.FillModeNonSolid, "wireframe");
		addPath(m_Info.PortabilitySubset, "portability subset");
		addPath(m_Info.BloomFormat == nvrhi::Format::R11G11B10_FLOAT, "B10G11R11 bloom");
		const std::string_view validation = !m_Info.Validation ? "off" : (m_Info.SynchronizationValidation ? "on, with synchronization" : "on");
		ENGINE_CORE_INFO("Graphics device: '{}' ({}, Vulkan {}, class {}, queue family {}); optional paths: {}; validation {}", m_Info.DeviceName,
			GpuDeviceTypeToString(m_Info.Type), VulkanApiVersionToString(m_Info.ApiVersion), m_Info.DeviceClass, m_Info.GraphicsQueueFamily,
			paths.empty() ? std::string("none") : paths, validation);
	}

	nvrhi::IDevice* GraphicsDevice::GetNvrhiDevice() const
	{
		return m_NvrhiDevice.Get();
	}

	const GraphicsDeviceInfo& GraphicsDevice::GetInfo() const
	{
		return m_Info;
	}

	const GraphicsSpecification& GraphicsDevice::GetSpecification() const
	{
		return m_Specification;
	}

	GpuDiagnostics& GraphicsDevice::GetDiagnostics()
	{
		return m_Diagnostics;
	}

	const GpuDiagnostics& GraphicsDevice::GetDiagnostics() const
	{
		return m_Diagnostics;
	}

	GpuResourceTracker& GraphicsDevice::GetResourceTracker()
	{
		return m_ResourceTracker;
	}

	const GpuResourceTracker& GraphicsDevice::GetResourceTracker() const
	{
		return m_ResourceTracker;
	}

	HostImageUpload& GraphicsDevice::GetHostImageUpload()
	{
		ENGINE_CORE_VERIFY(m_HostImageUpload != nullptr, "GraphicsDevice::GetHostImageUpload before the device was created");
		return *m_HostImageUpload;
	}

	Result<nvrhi::TextureHandle> GraphicsDevice::CreateTexture(const nvrhi::TextureDesc& desc)
	{
		if (m_Diagnostics.ShouldFailTextureCreation(desc))
		{
			return MakeError(ErrorCode::Gpu, "cannot create texture {}: out of device memory (injected by --gpu-inject-fault={})",
				Utils::DescribeTexture(desc), GpuFaultToString(GpuFault::OomTexture));
		}
		nvrhi::TextureHandle texture = m_NvrhiDevice->createTexture(desc);
		if (!texture)
			return MakeError(ErrorCode::Gpu, "cannot create texture {}", Utils::DescribeTexture(desc));
		m_ResourceTracker.Track(GpuResourceType::Texture, texture.Get());
		return texture;
	}

	Result<nvrhi::StagingTextureHandle> GraphicsDevice::CreateStagingTexture(const nvrhi::TextureDesc& desc, nvrhi::CpuAccessMode cpuAccess)
	{
		nvrhi::StagingTextureHandle texture = m_NvrhiDevice->createStagingTexture(desc, cpuAccess);
		if (!texture)
			return MakeError(ErrorCode::Gpu, "cannot create staging texture {}", Utils::DescribeTexture(desc));
		m_ResourceTracker.Track(GpuResourceType::StagingTexture, texture.Get());
		return texture;
	}

	Result<nvrhi::BufferHandle> GraphicsDevice::CreateBuffer(const nvrhi::BufferDesc& desc)
	{
		nvrhi::BufferHandle buffer = m_NvrhiDevice->createBuffer(desc);
		if (!buffer)
			return MakeError(ErrorCode::Gpu, "cannot create buffer '{}' ({} bytes)", Utils::DebugNameOrUnnamed(desc.debugName), desc.byteSize);
		m_ResourceTracker.Track(GpuResourceType::Buffer, buffer.Get());
		return buffer;
	}

	Result<nvrhi::SamplerHandle> GraphicsDevice::CreateSampler(const nvrhi::SamplerDesc& desc)
	{
		nvrhi::SamplerHandle sampler = m_NvrhiDevice->createSampler(desc);
		if (!sampler)
		{
			// Sampler descriptions carry no debug name.
			return MakeError(ErrorCode::Gpu, "cannot create sampler (filters {}/{}/{}, anisotropy {})", desc.minFilter ? "linear" : "point",
				desc.magFilter ? "linear" : "point", desc.mipFilter ? "linear" : "point", desc.maxAnisotropy);
		}
		m_ResourceTracker.Track(GpuResourceType::Sampler, sampler.Get());
		return sampler;
	}

	Result<nvrhi::ShaderHandle> GraphicsDevice::CreateShader(const nvrhi::ShaderDesc& desc, std::span<const std::byte> binary)
	{
		nvrhi::ShaderHandle shader = m_NvrhiDevice->createShader(desc, binary.data(), binary.size());
		if (!shader)
		{
			return MakeError(ErrorCode::Gpu, "cannot create shader '{}' (entry '{}', {} bytes)", Utils::DebugNameOrUnnamed(desc.debugName),
				desc.entryName, binary.size());
		}
		m_ResourceTracker.Track(GpuResourceType::Shader, shader.Get());
		return shader;
	}

	Result<nvrhi::ShaderHandle> GraphicsDevice::CreateShaderSpecialization(nvrhi::IShader& baseShader,
		std::span<const nvrhi::ShaderSpecialization> constants)
	{
		nvrhi::ShaderHandle shader =
			m_NvrhiDevice->createShaderSpecialization(&baseShader, constants.data(), static_cast<uint32_t>(constants.size()));
		if (!shader)
		{
			return MakeError(ErrorCode::Gpu, "cannot create a specialization of shader '{}' ({} constants)",
				Utils::DebugNameOrUnnamed(baseShader.getDesc().debugName), constants.size());
		}
		m_ResourceTracker.Track(GpuResourceType::Shader, shader.Get());
		return shader;
	}

	Result<nvrhi::InputLayoutHandle> GraphicsDevice::CreateInputLayout(std::span<const nvrhi::VertexAttributeDesc> attributes,
		nvrhi::IShader* vertexShader)
	{
		nvrhi::InputLayoutHandle layout =
			m_NvrhiDevice->createInputLayout(attributes.data(), static_cast<uint32_t>(attributes.size()), vertexShader);
		if (!layout)
		{
			return MakeError(ErrorCode::Gpu, "cannot create an input layout with {} attributes for shader '{}'", attributes.size(),
				Utils::GetShaderName(vertexShader));
		}
		m_ResourceTracker.Track(GpuResourceType::InputLayout, layout.Get());
		return layout;
	}

	Result<nvrhi::BindingLayoutHandle> GraphicsDevice::CreateBindingLayout(const nvrhi::BindingLayoutDesc& desc)
	{
		nvrhi::BindingLayoutHandle layout = m_NvrhiDevice->createBindingLayout(desc);
		if (!layout)
		{
			return MakeError(ErrorCode::Gpu, "cannot create a binding layout (space {}, {} bindings)", desc.registerSpace,
				desc.bindings.size());
		}
		m_ResourceTracker.Track(GpuResourceType::BindingLayout, layout.Get());
		return layout;
	}

	Result<nvrhi::BindingSetHandle> GraphicsDevice::CreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout& layout)
	{
		nvrhi::BindingSetHandle set = m_NvrhiDevice->createBindingSet(desc, &layout);
		if (!set)
			return MakeError(ErrorCode::Gpu, "cannot create a binding set ({} bindings)", desc.bindings.size());
		m_ResourceTracker.Track(GpuResourceType::BindingSet, set.Get());
		return set;
	}

	Result<nvrhi::FramebufferHandle> GraphicsDevice::CreateFramebuffer(const nvrhi::FramebufferDesc& desc)
	{
		nvrhi::FramebufferHandle framebuffer = m_NvrhiDevice->createFramebuffer(desc);
		if (!framebuffer)
		{
			// A framebuffer is named after its first attachment.
			const nvrhi::ITexture* first = !desc.colorAttachments.empty() ? desc.colorAttachments[0].texture : desc.depthAttachment.texture;
			return MakeError(ErrorCode::Gpu, "cannot create a framebuffer for '{}' ({} color attachments{})",
				first != nullptr ? Utils::DebugNameOrUnnamed(first->getDesc().debugName) : std::string_view("nothing"), desc.colorAttachments.size(),
				desc.depthAttachment.valid() ? ", depth" : "");
		}
		m_ResourceTracker.Track(GpuResourceType::Framebuffer, framebuffer.Get());
		return framebuffer;
	}

	Result<nvrhi::GraphicsPipelineHandle> GraphicsDevice::CreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& desc,
		const nvrhi::FramebufferInfo& framebufferInfo)
	{
		nvrhi::GraphicsPipelineHandle pipeline = m_NvrhiDevice->createGraphicsPipeline(desc, framebufferInfo);
		if (!pipeline)
		{
			return MakeError(ErrorCode::Gpu, "cannot create the graphics pipeline of shaders '{}' and '{}'", Utils::GetShaderName(desc.VS.Get()),
				Utils::GetShaderName(desc.PS.Get()));
		}
		m_ResourceTracker.Track(GpuResourceType::GraphicsPipeline, pipeline.Get());
		return pipeline;
	}

	Result<nvrhi::ComputePipelineHandle> GraphicsDevice::CreateComputePipeline(const nvrhi::ComputePipelineDesc& desc)
	{
		nvrhi::ComputePipelineHandle pipeline = m_NvrhiDevice->createComputePipeline(desc);
		if (!pipeline)
			return MakeError(ErrorCode::Gpu, "cannot create the compute pipeline of shader '{}'", Utils::GetShaderName(desc.CS.Get()));
		m_ResourceTracker.Track(GpuResourceType::ComputePipeline, pipeline.Get());
		return pipeline;
	}

	Result<nvrhi::CommandListHandle> GraphicsDevice::CreateCommandList(const nvrhi::CommandListParameters& parameters)
	{
		nvrhi::CommandListHandle commandList = m_NvrhiDevice->createCommandList(parameters);
		if (!commandList)
			return MakeError(ErrorCode::Gpu, "cannot create a command list");
		m_ResourceTracker.Track(GpuResourceType::CommandList, commandList.Get());
		return commandList;
	}

	Result<nvrhi::EventQueryHandle> GraphicsDevice::CreateEventQuery()
	{
		nvrhi::EventQueryHandle query = m_NvrhiDevice->createEventQuery();
		if (!query)
			return MakeError(ErrorCode::Gpu, "cannot create an event query");
		m_ResourceTracker.Track(GpuResourceType::EventQuery, query.Get());
		return query;
	}

	Result<nvrhi::TimerQueryHandle> GraphicsDevice::CreateTimerQuery()
	{
		nvrhi::TimerQueryHandle query = m_NvrhiDevice->createTimerQuery();
		if (!query)
			return MakeError(ErrorCode::Gpu, "cannot create a timer query (NVRHI's timer query pool may be exhausted)");
		m_ResourceTracker.Track(GpuResourceType::TimerQuery, query.Get());
		return query;
	}

	Result<nvrhi::TextureHandle> GraphicsDevice::CreateHandleForNativeTexture(VkImage image, const nvrhi::TextureDesc& desc)
	{
		// Not tracked (see the header): the image's creator counts it.
		nvrhi::TextureHandle texture = m_NvrhiDevice->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, nvrhi::Object(image), desc);
		if (!texture)
			return MakeError(ErrorCode::Gpu, "cannot wrap the native image of texture {}", Utils::DescribeTexture(desc));
		return texture;
	}

	uint64_t GraphicsDevice::ExecuteCommandList(nvrhi::ICommandList& commandList)
	{
		const std::array<nvrhi::ICommandList*, 1> lists = { &commandList };
		return ExecuteCommandLists(lists);
	}

	uint64_t GraphicsDevice::ExecuteCommandLists(std::span<nvrhi::ICommandList* const> commandLists)
	{
		ENGINE_CORE_ASSERT(!commandLists.empty(), "GraphicsDevice::ExecuteCommandLists without command lists");
		const uint64_t submission = m_NvrhiDevice->executeCommandLists(commandLists.data(), commandLists.size(), nvrhi::CommandQueue::Graphics);
		if (submission != 0)
			m_LastSubmissionID = submission;
		// NVRHI reports a device lost inside its submit through the message callback (§8.1); the injected fault likewise.
		m_Diagnostics.OnSubmitted();
		if (m_Diagnostics.IsDeviceLost())
			RaiseDeviceLost("executeCommandLists");
		return submission;
	}

	uint64_t GraphicsDevice::GetLastSubmissionID() const
	{
		return m_LastSubmissionID;
	}

	uint64_t GraphicsDevice::GetCompletedSubmissionID()
	{
		uint64_t value = 0;
		const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetSemaphoreCounterValue(m_Device, m_GraphicsQueueTimelineSemaphore, &value);
		if (result == VK_ERROR_DEVICE_LOST)
		{
			m_Diagnostics.SetDeviceLost();
			RaiseDeviceLost("vkGetSemaphoreCounterValue");
		}
		if (result != VK_SUCCESS)
			FatalError(GetFatalErrorKind(result), std::format("vkGetSemaphoreCounterValue failed: {}", VkResultToString(result)));
		return value;
	}

	void GraphicsDevice::RunGarbageCollection()
	{
		// §8.14 item 2, ADR 0009 decision 9: NVRHI retires the completed submissions first, so the sweep sees the
		// references they dropped, and the host images' release sees the sweep's.
		m_NvrhiDevice->runGarbageCollection();
		m_ResourceTracker.Sweep();
		if (m_HostImageUpload != nullptr)
			m_HostImageUpload->CollectGarbage();
	}

	void GraphicsDevice::WaitForIdle()
	{
		// NVRHI's waitForIdle is vkDeviceWaitIdle through vulkan.hpp's throwing wrapper, which catches only device loss.
		// The C entry point reports every result as a value, so this also works outside the frame boundary (shutdown, the
		// swapchain's destructor) and never throws (§4.6 item 2).
		const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkDeviceWaitIdle(m_Device);
		if (result == VK_ERROR_DEVICE_LOST || m_Diagnostics.IsDeviceLost())
			RaiseDeviceLost("vkDeviceWaitIdle");
		if (result != VK_SUCCESS)
			FatalError(GetFatalErrorKind(result), std::format("vkDeviceWaitIdle failed: {}", VkResultToString(result)));
	}

	void RaiseVulkanError(GraphicsDevice* device, VkResult result, std::string_view message)
	{
		if (result == VK_ERROR_DEVICE_LOST && device != nullptr)
			device->RaiseDeviceLost(message);
		FatalError(GetFatalErrorKind(result), message);
	}

	void GraphicsDevice::RaiseDeviceLost(std::string_view context)
	{
		m_Diagnostics.SetDeviceLost();
		std::string message = std::format("{}: device lost", context);
		if (m_Info.DeviceFault)
			message += std::format("; {}", DescribeDeviceFault());
		FatalError(FatalErrorKind::DeviceLost, message);
	}

	std::string GraphicsDevice::DescribeDeviceFault()
	{
		if (!m_Info.DeviceFault || m_Device == VK_NULL_HANDLE)
			return "VK_EXT_device_fault is not available";
		// The query is valid only on a lost device (validation reports it otherwise), and an injected loss has no fault.
		if (!m_Diagnostics.IsDeviceLost())
			return "no fault information";
		if (m_Diagnostics.GetInjectedFault() == GpuFault::DeviceLost)
			return std::format("no fault information (the device loss was injected by --gpu-inject-fault={})", GpuFaultToString(GpuFault::DeviceLost));
		const vk::detail::DispatchLoaderDynamic& dispatcher = VULKAN_HPP_DEFAULT_DISPATCHER;

		VkDeviceFaultCountsEXT counts{};
		counts.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT;
		VkResult result = dispatcher.vkGetDeviceFaultInfoEXT(m_Device, &counts, nullptr);
		if (result != VK_SUCCESS)
			return std::format("vkGetDeviceFaultInfoEXT failed: {}", VkResultToString(result));

		// The vendor binary is not requested: the report is text.
		std::vector<VkDeviceFaultAddressInfoEXT> addresses(counts.addressInfoCount);
		std::vector<VkDeviceFaultVendorInfoEXT> vendors(counts.vendorInfoCount);
		counts.vendorBinarySize = 0;
		VkDeviceFaultInfoEXT info{};
		info.sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT;
		info.pAddressInfos = addresses.empty() ? nullptr : addresses.data();
		info.pVendorInfos = vendors.empty() ? nullptr : vendors.data();
		result = dispatcher.vkGetDeviceFaultInfoEXT(m_Device, &counts, &info);
		if (result != VK_SUCCESS && result != VK_INCOMPLETE)
			return std::format("vkGetDeviceFaultInfoEXT failed: {}", VkResultToString(result));
		addresses.resize(std::min<size_t>(addresses.size(), counts.addressInfoCount));
		vendors.resize(std::min<size_t>(vendors.size(), counts.vendorInfoCount));

		const std::string_view description = Utils::FixedStringView(info.description);
		if (description.empty() && addresses.empty() && vendors.empty())
			return "no fault information";

		std::string text = std::format("device fault: '{}'", description);
		for (const VkDeviceFaultAddressInfoEXT& address : addresses)
		{
			text += std::format("; address {} 0x{:016x} (precision 0x{:x})", vk::to_string(vk::DeviceFaultAddressTypeEXT(address.addressType)),
				address.reportedAddress, address.addressPrecision);
		}
		for (const VkDeviceFaultVendorInfoEXT& vendor : vendors)
		{
			text += std::format("; vendor fault '{}' (code 0x{:x}, data 0x{:x})", Utils::FixedStringView(vendor.description), vendor.vendorFaultCode,
				vendor.vendorFaultData);
		}
		return text;
	}

	VkInstance GraphicsDevice::GetVulkanInstance() const
	{
		return m_Instance;
	}

	VkPhysicalDevice GraphicsDevice::GetVulkanPhysicalDevice() const
	{
		return m_PhysicalDevice;
	}

	VkDevice GraphicsDevice::GetVulkanDevice() const
	{
		return m_Device;
	}

	VkQueue GraphicsDevice::GetGraphicsQueue() const
	{
		return m_GraphicsQueue;
	}

	VkSemaphore GraphicsDevice::GetGraphicsQueueTimelineSemaphore() const
	{
		return m_GraphicsQueueTimelineSemaphore;
	}

	void GraphicsDevice::QueueWaitForSemaphore(VkSemaphore semaphore, uint64_t value)
	{
		static_cast<nvrhi::vulkan::IDevice*>(m_VulkanNvrhiDevice.Get())->queueWaitForSemaphore(nvrhi::CommandQueue::Graphics, semaphore, value);
	}

	void GraphicsDevice::QueueSignalSemaphore(VkSemaphore semaphore, uint64_t value)
	{
		static_cast<nvrhi::vulkan::IDevice*>(m_VulkanNvrhiDevice.Get())->queueSignalSemaphore(nvrhi::CommandQueue::Graphics, semaphore, value);
	}

}
