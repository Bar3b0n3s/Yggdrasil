#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/GlfwLibrary.h"

#include <cstdint>
#include <string_view>

// The process's Vulkan loader and vulkan.hpp's default dispatcher (Architecture §3 rule 5, §4.1 level 1, §8.1). The
// dispatcher is process-level state: exactly one translation unit, Graphics/VulkanDispatch.cpp, holds its storage
// (VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE), and ProcessContext initializes the loader once, before GLFW, so
// GLFW uses the engine's loader (glfwInitVulkanLoader). The per-context half of the init order, init(instance) and
// init(device), is GraphicsDevice::Create's.

namespace Engine {

	// The message every "no Vulkan loader" error carries (§4.6 item 1); RunApplication prints it and exits with code 3.
	inline constexpr std::string_view NoVulkanLoaderMessage = "No Vulkan loader found; install a GPU driver with Vulkan 1.3 support";

	// The environment variable that simulates a missing loader in non-Dist builds (Roadmap M5 test hook): with the value
	// "missing", Initialize fails exactly as if the loader library did not exist. Any other non-empty value is
	// InvalidArgument, so a misspelled hook never passes silently. Dist builds ignore the variable.
	inline constexpr std::string_view VulkanLoaderEnvironmentVariable = "ENGINE_VULKAN_LOADER";

	// Static access to the process's Vulkan loader. Main thread only (asserted in Debug and Release builds).
	class VulkanDispatch
	{
	public:
		VulkanDispatch() = delete;

		// In order: honours ENGINE_VULKAN_LOADER (non-Dist), creates the vk::detail::DynamicLoader (vulkan-1.dll,
		// libvulkan.so.1 or libvulkan.1.dylib), and VULKAN_HPP_DEFAULT_DISPATCHER.init(vkGetInstanceProcAddr). The loader
		// stays loaded until Shutdown, which ProcessContext calls after the last GraphicsDevice (and so the last
		// vkDestroyInstance) is gone. Initializing twice without Shutdown is a programmer error (asserted). Errors:
		// Unsupported with NoVulkanLoaderMessage when no loader library exists (the std::runtime_error of DynamicLoader is
		// caught here, §4.6) or when it lacks vkGetInstanceProcAddr; InvalidArgument for a bad ENGINE_VULKAN_LOADER value.
		// Nothing stays loaded on failure.
		[[nodiscard]] static Status Initialize();

		// Unloads the loader and resets the default dispatcher. Every GraphicsDevice must be destroyed before (asserted).
		// Idempotent.
		static void Shutdown();

		[[nodiscard]] static bool IsInitialized();

		// The loader's vkGetInstanceProcAddr, spelled as GlfwLibrary's alias so that ProcessContext hands it to
		// glfwInitVulkanLoader without the Vulkan headers; nullptr when the loader is not initialized.
		[[nodiscard]] static VulkanGetInstanceProcAddrFunction GetInstanceProcAddr();

		// vk::enumerateInstanceVersion() as VK_MAKE_API_VERSION-encoded value (variant, major, minor, patch); 0 when the
		// loader is not initialized. The loader of a Vulkan 1.0 implementation, which lacks the function, reports 1.0.
		[[nodiscard]] static uint32_t GetLoaderApiVersion();
	};

}
