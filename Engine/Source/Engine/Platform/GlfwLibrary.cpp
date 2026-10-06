#include "EnginePCH.h"
#include "Engine/Platform/GlfwLibrary.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"

// Order matters: glfw3.h declares glfwInitVulkanLoader only when the Vulkan headers come first (ADR 0005 decision 8).
// vulkan_core.h declares the Vulkan types without any OS header.
#include <vulkan/vulkan_core.h>
#include <GLFW/glfw3.h>

#include <thread>

namespace Engine {

	// GLFW's library state is process-level state (Architecture §3 rule 5): ProcessContext initializes it once, on the
	// process's main thread, and only that thread touches it.
	static bool s_IsInitialized = false;
	static WindowMode s_Mode = WindowMode::Windowed;
	static GlfwPlatform s_Platform = GlfwPlatform::None;
	static std::thread::id s_MainThread;
	static uint32_t s_WindowCount = 0;

	// VkInstance is a dispatchable handle, a pointer, so VulkanGetInstanceProcAddrFunction and PFN_vkGetInstanceProcAddr
	// have the same parameters as far as the ABI goes; the engine hands GLFW the loader's own vkGetInstanceProcAddr.
	static_assert(std::is_pointer_v<VkInstance>);
	static_assert(sizeof(VulkanGetInstanceProcAddrFunction) == sizeof(PFN_vkGetInstanceProcAddr));

	namespace Utils {

		static void OnGlfwError(int code, const char* description)
		{
			ENGINE_CORE_ERROR("GLFW error 0x{:08X}: {}", code, description != nullptr ? description : "(no description)");
		}

		static GlfwPlatform ToGlfwPlatform(int platform)
		{
			switch (platform)
			{
				case GLFW_PLATFORM_NULL:  return GlfwPlatform::Null;
				case GLFW_PLATFORM_WIN32: return GlfwPlatform::Win32;
				case GLFW_PLATFORM_COCOA: return GlfwPlatform::Cocoa;
				case GLFW_PLATFORM_X11:   return GlfwPlatform::X11;
				default:                  break;
			}

			// Wayland is not built (Vendor/GLFW/VENDOR.md), so GLFW cannot report it.
			ENGINE_CORE_ASSERT(false, "GLFW runs on an unexpected platform 0x{:08X}", platform);
			return GlfwPlatform::None;
		}

	}

	Status GlfwLibrary::Initialize(const GlfwLibrarySpecification& specification)
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized, "GLFW is already initialized: GlfwLibrary::Initialize called twice without Shutdown");
		if (s_IsInitialized)
			return MakeError(ErrorCode::InvalidState, "GLFW is already initialized");

		// Discard an error left over from calls before initialization, so that a failure below reports its own.
		static_cast<void>(glfwGetError(nullptr));
		glfwSetErrorCallback(&Utils::OnGlfwError);

		// Init hints outlive glfwTerminate, so every hint this function relies on is set on every call. A null loader hands
		// loading back to GLFW, which then finds the Vulkan loader itself.
		glfwInitVulkanLoader(reinterpret_cast<PFN_vkGetInstanceProcAddr>(specification.VulkanLoader));
		// The null platform only when asked for it; GLFW_ANY_PLATFORM picks the one native platform this binary is built
		// with (Win32, Cocoa or X11) and never falls back to the null one.
		glfwInitHint(GLFW_PLATFORM, specification.Mode == WindowMode::Headless ? GLFW_PLATFORM_NULL : GLFW_ANY_PLATFORM);
		// Keep the working directory: on macOS GLFW would otherwise move it into an application bundle's Resources folder.
		glfwInitHint(GLFW_COCOA_CHDIR_RESOURCES, GLFW_FALSE);

		if (glfwInit() != GLFW_TRUE)
		{
			const bool isHeadless = specification.Mode == WindowMode::Headless;
			std::string message = std::format("GLFW cannot initialize its {} platform: {}", isHeadless ? "null" : "native",
				TakeErrorDescription());
			glfwSetErrorCallback(nullptr);
			if (isHeadless)
				return std::unexpected(Error(ErrorCode::Unsupported, std::move(message)));
			return std::unexpected(Error(ErrorCode::Unsupported, std::move(message))
					.WithHint("without a display, run with --headless, which uses GLFW's null platform"));
		}

		s_IsInitialized = true;
		s_Mode = specification.Mode;
		s_Platform = Utils::ToGlfwPlatform(glfwGetPlatform());
		s_MainThread = std::this_thread::get_id();
		ENGINE_CORE_INFO("GLFW initialized on the {} platform ({})", GlfwPlatformToString(s_Platform), glfwGetVersionString());
		return {};
	}

	void GlfwLibrary::Shutdown()
	{
		if (!s_IsInitialized)
			return;

		ENGINE_CORE_ASSERT(IsMainThread(), "GlfwLibrary::Shutdown called off the main thread");
		// glfwTerminate destroys the remaining windows, and a live Window would later destroy its handle a second time.
		ENGINE_CORE_VERIFY(s_WindowCount == 0, "GlfwLibrary::Shutdown with {} live Window(s): destroy every Window first",
			s_WindowCount);

		glfwTerminate();
		glfwSetErrorCallback(nullptr);
		s_IsInitialized = false;
		s_Mode = WindowMode::Windowed;
		s_Platform = GlfwPlatform::None;
		s_MainThread = std::thread::id();
	}

	bool GlfwLibrary::IsInitialized()
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized || IsMainThread(), "GlfwLibrary::IsInitialized called off the main thread");
		return s_IsInitialized;
	}

	GlfwPlatform GlfwLibrary::GetPlatform()
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized || IsMainThread(), "GlfwLibrary::GetPlatform called off the main thread");
		return s_Platform;
	}

	WindowMode GlfwLibrary::GetMode()
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized || IsMainThread(), "GlfwLibrary::GetMode called off the main thread");
		return s_Mode;
	}

	uint32_t GlfwLibrary::GetWindowCount()
	{
		ENGINE_CORE_ASSERT(!s_IsInitialized || IsMainThread(), "GlfwLibrary::GetWindowCount called off the main thread");
		return s_WindowCount;
	}

	void GlfwLibrary::RegisterWindow()
	{
		ENGINE_CORE_ASSERT(IsMainThread(), "A Window was created off the main thread or without GLFW");
		++s_WindowCount;
	}

	void GlfwLibrary::UnregisterWindow()
	{
		ENGINE_CORE_ASSERT(s_WindowCount > 0, "GlfwLibrary::UnregisterWindow without a registered Window");
		if (s_WindowCount > 0)
			--s_WindowCount;
	}

	bool GlfwLibrary::IsMainThread()
	{
		return s_IsInitialized && std::this_thread::get_id() == s_MainThread;
	}

	std::string GlfwLibrary::TakeErrorDescription()
	{
		const char* description = nullptr;
		const int code = glfwGetError(&description);
		if (code == GLFW_NO_ERROR)
			return "GLFW reported no error";
		return std::format("{} (GLFW error 0x{:08X})", description != nullptr ? description : "no description", code);
	}

	std::string_view WindowModeToString(WindowMode mode)
	{
		switch (mode)
		{
			case WindowMode::Windowed: return "Windowed";
			case WindowMode::Headless: return "Headless";
		}

		ENGINE_CORE_ASSERT(false, "Unknown WindowMode {}", std::to_underlying(mode));
		return "Unknown";
	}

	std::string_view GlfwPlatformToString(GlfwPlatform platform)
	{
		switch (platform)
		{
			case GlfwPlatform::None:  return "None";
			case GlfwPlatform::Null:  return "Null";
			case GlfwPlatform::Win32: return "Win32";
			case GlfwPlatform::Cocoa: return "Cocoa";
			case GlfwPlatform::X11:   return "X11";
		}

		ENGINE_CORE_ASSERT(false, "Unknown GlfwPlatform {}", std::to_underlying(platform));
		return "Unknown";
	}

}
