-- libogg (https://github.com/xiph/ogg), vendored at v1.3.6 (commit be05b13e98b048f0b5a0f5fa8ce514d56db5f822). See
-- VENDOR.md. Upstream builds it with CMake (CMakeLists.txt: add_library(ogg ...)) or autotools; this project compiles
-- the same two sources, unmodified, as one static library: the Ogg container's bit packer and page framing, which
-- opusfile reads Ogg Opus files with (M23, Docs/Decisions/0034-m23-contract.md decision 16).
--
-- Consumers (Vendor/opusfile and miniaudio's libopus decoder, Vendor/miniaudio) add include as an include directory and
-- include <ogg/ogg.h>. The library has no configuration defines; no first-party code includes its headers
-- (Dependencies.lua UseOgg() only links it).

project "ogg"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Mirrors OGG_HEADERS and OGG_SOURCES of upstream's CMakeLists.txt; include/ogg/config_types.h is the configured
	-- header CMake writes into its build tree (VENDOR.md, "Local additions").
	files
	{
		"include/ogg/config_types.h",
		"include/ogg/ogg.h",
		"include/ogg/os_types.h",
		"src/bitwise.c",
		"src/crctable.h",
		"src/framing.c",
	}

	-- target_include_directories(ogg PUBLIC include).
	includedirs
	{
		"include",
	}

	filter "system:windows"
		systemversion "latest"

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
