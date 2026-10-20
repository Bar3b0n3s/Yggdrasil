-- nativefiledialog-extended v1.4.1 (https://github.com/btzy/nativefiledialog-extended), vendored at commit
-- 65b2f070a0fcfee0d453a64e770a217ab0b6ff4f. See VENDOR.md. The file list mirrors upstream src/CMakeLists.txt for a static
-- library with its Windows backend only (Docs/Decisions/0029-m18-contract.md decision 21; Docs/Decisions/0028-backlog-program.md
-- decision 10, "Windows backends only"): nfd_win.cpp, the COM IFileDialog backend, is listed only on Windows. The GTK, portal
-- and Cocoa backends are not vendored, and the Linux and macOS branches list no backend source and link no system library:
-- the project exists on every OS only so that the portability stage's per-configuration project sets match the vs2026
-- reference (Scripts/CI.py check_portability); nothing compiles it there.
--
-- No configuration macro: the defaults (UTF-8 and native entry points, no portal, NFD_OVERRIDE_RECENT_WITH_DEFAULT off),
-- so the library has no ABI-relevant define and its consumers none. Consumers: UseNativeFileDialog() in Dependencies.lua
-- (the include directory for Engine, whose Platform/Windows/NativeFileDialogWindows.cpp alone includes nfd.h, and the
-- library with its system libraries for the Editor and Tests executables, on Windows only).

project "nfd"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"src/include/nfd.h"
	}

	includedirs
	{
		"src/include"
	}

	filter "system:windows"
		systemversion "latest"

		files
		{
			"src/nfd_win.cpp"
		}

	filter "system:linux"
		pic "On"

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
