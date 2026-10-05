-- spdlog (https://github.com/gabime/spdlog), vendored at v1.17.0. See VENDOR.md.
-- Compiled-library build (SPDLOG_COMPILED_LIB) that formats with C++20/23 std::format
-- (SPDLOG_USE_STD_FORMAT) instead of the bundled fmt library.
--
-- Consumers MUST use the same public defines (see VENDOR.md):
--     defines { "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT" }
--     includedirs { "%{wks.location}/Vendor/spdlog/include" }   (adjust to the workspace layout)
-- and should compile with the same C++ dialect (C++23) and, on MSVC, with /utf-8.

project "spdlog"
	kind "StaticLib"
	language "C++"
	cppdialect "C++23"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Mirrors SPDLOG_SRCS in upstream CMakeLists.txt for SPDLOG_USE_STD_FORMAT=ON
	-- (src/bundled_fmtlib_format.cpp is only compiled when the bundled fmt is used).
	files
	{
		"include/spdlog/**.h",
		"src/spdlog.cpp",
		"src/stdout_sinks.cpp",
		"src/color_sinks.cpp",
		"src/file_sinks.cpp",
		"src/async.cpp",
		"src/cfg.cpp",
	}

	includedirs
	{
		"include",
	}

	-- PUBLIC defines in upstream CMake: every consumer must define these as well.
	defines
	{
		"SPDLOG_COMPILED_LIB",
		"SPDLOG_USE_STD_FORMAT",
	}

	-- Upstream adds these MSVC options: /utf-8 (PUBLIC, enabled by default via SPDLOG_MSVC_UTF8) and
	-- /Zc:__cplusplus (PRIVATE for the compiled library) so that __cplusplus-based feature checks in
	-- the spdlog headers see the real language level.
	filter "toolset:msc*"
		buildoptions { "/utf-8", "/Zc:__cplusplus" }

	filter "system:windows"
		systemversion "latest"
		-- Upstream enables this PRIVATE define when _fwrite_nolock is available (always on the MSVC CRT).
		defines { "SPDLOG_FWRITE_UNLOCKED" }

	filter "system:linux"
		pic "On"
		-- Upstream enables this PRIVATE define when fwrite_unlocked is available (glibc).
		defines { "SPDLOG_FWRITE_UNLOCKED" }

	-- macOS: libc has no fwrite_unlocked, so SPDLOG_FWRITE_UNLOCKED is intentionally not defined
	-- (upstream's check_symbol_exists() fails there and leaves it off).

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
