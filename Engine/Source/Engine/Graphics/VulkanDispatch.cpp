#include "EnginePCH.h"
#include "Engine/Graphics/VulkanDispatch.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Platform/Environment.h"

#include <vulkan/vulkan.hpp>

#include <optional>
#include <stdexcept>
#include <thread>

// The process's Vulkan loader (Architecture §4.1 level 1, §8.1). vk::detail::DynamicLoader throws std::runtime_error when
// no loader library exists; this file is one of the allowlisted boundaries of §4.6 that turns that exception into a
// Result (ADR 0009 decision 1).

namespace Engine {

	namespace Utils {

		// Process-level state (§3 rule 5): the loaded library, kept alive until Shutdown, and the thread that loaded it.
		static std::optional<vk::detail::DynamicLoader> s_VulkanLoader;
		static PFN_vkGetInstanceProcAddr s_GetInstanceProcAddr = nullptr;
		static std::thread::id s_VulkanLoaderThread;
		// The live GraphicsDevice objects (at most one, GraphicsDevice.h), which must be gone before the loader unloads.
		static uint32_t s_VulkanDeviceCount = 0;

		// The ENGINE_VULKAN_LOADER test hook (Roadmap M5): true when it asks for a missing loader. Dist ignores it.
		static Result<bool> IsMissingLoaderRequested()
		{
			if constexpr (GpuTestHooksEnabled)
			{
				const std::optional<std::string> value = ReadEnvironmentVariable(VulkanLoaderEnvironmentVariable);
				if (!value.has_value() || value->empty())
					return false;
				if (*value == "missing")
					return true;
				return std::unexpected(Error(ErrorCode::InvalidArgument,
					std::format("'{}' is not a valid value of {}", *value, VulkanLoaderEnvironmentVariable))
						.WithHint(std::format("set {}=missing to simulate a missing Vulkan loader, or unset it", VulkanLoaderEnvironmentVariable)));
			}
			else
			{
				return false;
			}
		}

		// Loads the loader library. The one catch of this file (§4.6 item 1): the exception never leaves it.
		static std::optional<vk::detail::DynamicLoader> LoadVulkanLibrary(std::string& failure)
		{
			try
			{
				return std::optional<vk::detail::DynamicLoader>(std::in_place);
			}
			catch (const std::runtime_error& error)
			{
				failure = error.what();
				return std::nullopt;
			}
		}

	}

	Status VulkanDispatch::Initialize()
	{
		ENGINE_CORE_ASSERT(!Utils::s_VulkanLoader.has_value(), "VulkanDispatch::Initialize called twice without Shutdown");

		ENGINE_TRY_ASSIGN(const bool simulateMissing, Utils::IsMissingLoaderRequested());
		if (simulateMissing)
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::string(NoVulkanLoaderMessage))
					.WithContext(std::format("while loading the Vulkan loader ({}=missing simulates its absence)", VulkanLoaderEnvironmentVariable)));
		}

		std::string failure;
		std::optional<vk::detail::DynamicLoader> loader = Utils::LoadVulkanLibrary(failure);
		if (!loader.has_value())
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::string(NoVulkanLoaderMessage))
					.WithContext(std::format("while loading the Vulkan loader ({})", failure)));
		}

		const auto getInstanceProcAddr = loader->getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
		if (getInstanceProcAddr == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Unsupported, std::string(NoVulkanLoaderMessage))
					.WithContext("while loading the Vulkan loader (the library has no vkGetInstanceProcAddr)"));
		}

		// The global commands (vkCreateInstance, vkEnumerateInstanceVersion, ...); init(instance) and init(device) follow
		// per context in GraphicsDevice::Create.
		VULKAN_HPP_DEFAULT_DISPATCHER.init(getInstanceProcAddr);
		Utils::s_VulkanLoader = std::move(loader);
		Utils::s_GetInstanceProcAddr = getInstanceProcAddr;
		Utils::s_VulkanLoaderThread = std::this_thread::get_id();

		const uint32_t version = GetLoaderApiVersion();
		ENGINE_CORE_INFO("Vulkan loader {}.{}.{} loaded", VK_API_VERSION_MAJOR(version), VK_API_VERSION_MINOR(version),
			VK_API_VERSION_PATCH(version));
		return {};
	}

	void VulkanDispatch::Shutdown()
	{
		if (!Utils::s_VulkanLoader.has_value())
			return;
		ENGINE_CORE_ASSERT(Utils::s_VulkanLoaderThread == std::this_thread::get_id(), "VulkanDispatch is main-thread-only");
		ENGINE_CORE_ASSERT(Utils::s_VulkanDeviceCount == 0, "VulkanDispatch::Shutdown while a GraphicsDevice exists");

		// No function pointer into the library may outlive it.
		VULKAN_HPP_DEFAULT_DISPATCHER = vk::detail::DispatchLoaderDynamic();
		Utils::s_GetInstanceProcAddr = nullptr;
		Utils::s_VulkanLoader.reset();
		Utils::s_VulkanLoaderThread = std::thread::id();
	}

	bool VulkanDispatch::IsInitialized()
	{
		return Utils::s_VulkanLoader.has_value();
	}

	VulkanGetInstanceProcAddrFunction VulkanDispatch::GetInstanceProcAddr()
	{
		// The same function under GlfwLibrary's spelling, which avoids the Vulkan headers (a native handle, §8 casts).
		return reinterpret_cast<VulkanGetInstanceProcAddrFunction>(Utils::s_GetInstanceProcAddr);
	}

	uint32_t VulkanDispatch::GetLoaderApiVersion()
	{
		if (!Utils::s_VulkanLoader.has_value())
			return 0;
		ENGINE_CORE_ASSERT(Utils::s_VulkanLoaderThread == std::this_thread::get_id(), "VulkanDispatch is main-thread-only");

		// A Vulkan 1.0 loader lacks the function. The C entry point reports failures as a VkResult instead of throwing.
		if (VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion == nullptr)
			return VK_API_VERSION_1_0;
		uint32_t version = 0;
		const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkEnumerateInstanceVersion(&version);
		if (result != VK_SUCCESS)
		{
			// Its only failure is VK_ERROR_OUT_OF_HOST_MEMORY; reporting 1.0 makes device creation refuse the loader.
			ENGINE_CORE_WARN("vkEnumerateInstanceVersion failed ({}); treating the Vulkan loader as 1.0", VkResultToString(result));
			return VK_API_VERSION_1_0;
		}
		return version;
	}

	void VulkanDispatch::RegisterDevice()
	{
		ENGINE_CORE_ASSERT(Utils::s_VulkanLoader.has_value(), "a GraphicsDevice needs an initialized VulkanDispatch");
		++Utils::s_VulkanDeviceCount;
	}

	void VulkanDispatch::UnregisterDevice()
	{
		ENGINE_CORE_ASSERT(Utils::s_VulkanDeviceCount > 0, "VulkanDispatch::UnregisterDevice without a registered device");
		--Utils::s_VulkanDeviceCount;
	}

	bool VulkanDispatch::HasDevice()
	{
		return Utils::s_VulkanDeviceCount > 0;
	}

}

// The one translation unit with the storage of vulkan.hpp's default dispatcher (Architecture §8.1; Vendor/NVRHI/VENDOR.md
// "Dynamic dispatcher"): NVRHI and every engine Vulkan call dispatch through it. The macro defines
// vk::detail::defaultDispatchLoaderDynamic at namespace scope.
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
