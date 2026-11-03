-- opusfile (https://github.com/xiph/opusfile), vendored at v0.12 (commit a55c164e9891a9326188b7d4d216ec9a88373739). See
-- VENDOR.md. Upstream builds it with autotools (Makefile.am: libopusfile_la_SOURCES) or its Visual Studio project
-- (win32/VS2015/opusfile.vcxproj); this project compiles the same four sources, unmodified, as one static library: the
-- Ogg Opus file reader miniaudio's libopus decoder opens Opus files with (M23, Docs/Decisions/0034-m23-contract.md
-- decision 16). libopusurl (http.c, wincerts.c: Opus streams over HTTP) is not vendored.
--
-- OP_FIXED_POINT (upstream's --enable-fixed-point) matches libopus's FIXED_POINT build: opusfile decodes 16-bit samples
-- with opus_multistream_decode and never dithers, op_read returns them as decoded, and op_read_float converts them
-- exactly (x / 32768), so the samples are bit-exact in every configuration. It is read only by the library's own
-- sources (src/internal.h), never by opusfile.h.
--
-- Consumers (miniaudio's libopus decoder, Vendor/miniaudio) add include, ../libogg/include and ../libopus/include as
-- include directories and include <opusfile.h>, with no defines. No first-party code includes the header
-- (Dependencies.lua UseOpusFile() only links it).

project "opusfile"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Mirrors libopusfile_la_SOURCES and its headers in upstream's Makefile.am.
	files
	{
		"include/opusfile.h",
		"src/info.c",
		"src/internal.c",
		"src/internal.h",
		"src/opusfile.c",
		"src/stream.c",
	}

	-- The library's public header, and libogg's and libopus's, which opusfile.h includes.
	includedirs
	{
		"include",
		"../libogg/include",
		"../libopus/include",
	}

	defines
	{
		"OP_FIXED_POINT",
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
