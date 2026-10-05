-- GLFW 3.5.1 (https://github.com/glfw/glfw) - see VENDOR.md
-- File lists and defines mirror upstream src/CMakeLists.txt for a static library build:
--   Windows: Win32 backend, Linux: X11 backend (Wayland not built), macOS: Cocoa backend.
-- The null backend, EGL/OSMesa context code and the Vulkan loader glue are built on every platform.

project "GLFW"
	kind "StaticLib"
	language "C"
	cdialect "C99"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"include/GLFW/glfw3.h",
		"include/GLFW/glfw3native.h",

		"src/internal.h",
		"src/platform.h",
		"src/mappings.h",
		"src/context.c",
		"src/init.c",
		"src/input.c",
		"src/monitor.c",
		"src/platform.c",
		"src/vulkan.c",
		"src/window.c",
		"src/egl_context.c",
		"src/osmesa_context.c",
		"src/null_platform.h",
		"src/null_joystick.h",
		"src/null_init.c",
		"src/null_monitor.c",
		"src/null_window.c",
		"src/null_joystick.c"
	}

	includedirs
	{
		"include"
	}

	filter "system:windows"
		systemversion "latest"

		files
		{
			-- Shared OS support (time, thread, module)
			"src/win32_time.h",
			"src/win32_thread.h",
			"src/win32_module.c",
			"src/win32_time.c",
			"src/win32_thread.c",

			-- Win32 backend
			"src/win32_platform.h",
			"src/win32_joystick.h",
			"src/win32_init.c",
			"src/win32_joystick.c",
			"src/win32_monitor.c",
			"src/win32_window.c",
			"src/wgl_context.c"
		}

		defines
		{
			"_GLFW_WIN32",
			"UNICODE",
			"_UNICODE",
			"_CRT_SECURE_NO_WARNINGS"
		}

	filter "system:linux"
		pic "On"

		files
		{
			-- Shared OS support (time, thread, module)
			"src/posix_time.h",
			"src/posix_thread.h",
			"src/posix_module.c",
			"src/posix_time.c",
			"src/posix_thread.c",

			-- X11 backend
			"src/x11_platform.h",
			"src/x11_init.c",
			"src/x11_monitor.c",
			"src/x11_window.c",
			"src/xkb_unicode.c",
			"src/glx_context.c",

			-- Shared by the X11 and Wayland backends
			"src/linux_joystick.h",
			"src/linux_joystick.c",
			"src/posix_poll.h",
			"src/posix_poll.c"
		}

		defines
		{
			"_GLFW_X11",
			-- Upstream: -std=c99 disables _DEFAULT_SOURCE (POSIX 2008 and more) on Linux
			"_DEFAULT_SOURCE"
		}

	filter "system:macosx"
		files
		{
			-- Shared OS support (time, thread, module)
			"src/macos_time.h",
			"src/macos_time.c",
			"src/posix_thread.h",
			"src/posix_module.c",
			"src/posix_thread.c",

			-- Cocoa backend
			"src/cocoa_platform.h",
			"src/cocoa_joystick.h",
			"src/cocoa_init.m",
			"src/cocoa_joystick.m",
			"src/cocoa_monitor.m",
			"src/cocoa_window.m",
			"src/nsgl_context.m"
		}

		defines
		{
			"_GLFW_COCOA"
		}

	filter { "system:macosx", "files:**.m" }
		compileas "Objective-C"

	filter "configurations:Debug"
		runtime "Debug"
		symbols "on"

	filter "configurations:Release"
		runtime "Release"
		optimize "on"
		symbols "on"

	filter "configurations:Dist"
		runtime "Release"
		optimize "full"
		symbols "off"

	filter {}
