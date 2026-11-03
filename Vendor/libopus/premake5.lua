-- libopus (https://github.com/xiph/opus), vendored at v1.6.1 (commit 22244de5a79bd1d6d623c32e72bf1954b56235be). See
-- VENDOR.md. Upstream builds it with CMake (CMakeLists.txt: add_library(opus ...)), meson or autotools; this project
-- compiles the same sources, unmodified, as one static library in upstream's fixed-point configuration: the Opus codec
-- that opusfile decodes Ogg Opus files with (M23, Docs/Decisions/0034-m23-contract.md decision 16).
--
-- Bit-exact decoding: FIXED_POINT (integer arithmetic throughout the decoder), no run-time CPU detection and no
-- intrinsics (OPUS_HAVE_RTCD and every OPUS_X86_* and OPUS_ARM_* macro stay undefined, and the x86, ARM and MIPS
-- sources are not vendored), and no deep-learning features (ENABLE_DEEP_PLC, ENABLE_DRED and ENABLE_OSCE stay
-- undefined, as upstream's defaults leave them; the dnn sources are not vendored). The decoded samples therefore do not
-- depend on the configuration or the CPU.
--
-- Consumers (Vendor/opusfile and miniaudio's libopus decoder, Vendor/miniaudio) add include as an include directory and
-- include <opus_multistream.h>. OPUS_BUILD, which the public headers read, is the library's own (its sources only); a
-- consumer defines none of the macros the public headers read (CUSTOM_MODES, ENABLE_OPUS_CUSTOM_API,
-- DISABLE_PTR_CHECK, DLL_EXPORT). No first-party code includes the headers (Dependencies.lua UseOpus() only links it).

project "opus"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Upstream's lists for this configuration (OpusSources.cmake reads the .mk files of the release): CELT_SOURCES,
	-- SILK_SOURCES and SILK_SOURCES_FIXED, OPUS_SOURCES and OPUS_SOURCES_FLOAT (always compiled upstream; the encoder's
	-- analysis), with their headers. The vendored tree holds exactly those sources, so the patterns name them all.
	files
	{
		"include/*.h",
		"celt/*.h",
		"celt/*.c",
		"silk/*.h",
		"silk/*.c",
		"silk/fixed/*.h",
		"silk/fixed/*.c",
		"src/*.h",
		"src/*.c",
	}

	-- target_include_directories(opus PUBLIC include PRIVATE . celt silk) and, for FIXED_POINT, silk/fixed.
	includedirs
	{
		"include",
		".",
		"celt",
		"silk",
		"silk/fixed",
	}

	-- Upstream's CMake definitions for this configuration: OPUS_BUILD; FIXED_POINT=1 (OPUS_FIXED_POINT); USE_ALLOCA, the
	-- stack-array choice where the C compiler has no variable-length arrays (MSVC) and the one used everywhere here so
	-- every platform decodes through the same code; ENABLE_HARDENING (OPUS_HARDENING, on by default: its checks end the
	-- process instead of continuing with a broken invariant); HAVE_LRINT and HAVE_LRINTF (the C99 functions its checks
	-- find on every supported compiler). config.h (HAVE_CONFIG_H) is not generated: it would only define
	-- PACKAGE_VERSION, which celt.c defaults to "unknown".
	defines
	{
		"OPUS_BUILD",
		"FIXED_POINT=1",
		"USE_ALLOCA",
		"ENABLE_HARDENING",
		"HAVE_LRINT",
		"HAVE_LRINTF",
	}

	filter "system:windows"
		systemversion "latest"

	-- MSVC at the /W3 vendored projects build with: C4244 (a 64-bit value narrowed to 32 bits) in the four sources where
	-- it shows. The fixed-point code narrows 64-bit intermediate sums and pointer differences to the 32-bit types its
	-- results have by design, and upstream's own CMake moves C4244 to level 4 for every MSVC build ("move cosmetic warnings
	-- to level 4", cmake/OpusConfig.cmake). Disabled for those translation units only (VENDOR.md, "Build configuration").
	filter { "toolset:msc*", "files:celt/quant_bands.c or files:celt/vq.c or files:src/extensions.c or files:src/mapping_matrix.c" }
		disablewarnings { "4244" }

	-- MSVC's C4293 in the multistream encoder's fixed-point log-sum (line 216) is on the branch of celt/fixed_generic.h's
	-- VSHR32(a, shift) that a positive constant shift never takes: VSHR32(..., DB_SHIFT-16) with DB_SHIFT 24 (celt/arch.h)
	-- is SHR32(..., 8), and MSVC warns about the untaken SHL32(..., -8). The engine never encodes. Disabled for that
	-- translation unit only (VENDOR.md, "Build configuration").
	filter { "toolset:msc*", "files:src/opus_multistream_encoder.c" }
		disablewarnings { "4293" }

	-- Upstream: C11 for MSVC (clang-cl included), C99 elsewhere; check_include_file(alloca.h HAVE_ALLOCA_H) where it exists.
	filter "action:vs*"
		cdialect "C11"

	filter "action:not vs*"
		cdialect "C99"

	filter "system:linux or macosx"
		defines { "HAVE_ALLOCA_H" }

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
