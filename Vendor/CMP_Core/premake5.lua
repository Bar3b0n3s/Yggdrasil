-- Compressonator CMP_Core (https://github.com/GPUOpen-Tools/compressonator, the cmp_core sources), vendored at V4.6.0
-- (commit 095a143ad8cfbf9ed5afa4ea63c735bfd0fd04dd). See VENDOR.md. Upstream builds it with CMake
-- (cmp_core/CMakeLists.txt); this project compiles the same sources, unmodified, as one static library: upstream's BC1
-- to BC7 block codecs (cmp_core/shaders/bc*_encode_kernel.cpp; the engine uses BC1, BC4, BC5, BC6H and BC7:
-- Docs/Decisions/0034-m23-contract.md decision 13), the API (cmp_core/source/cmp_core.cpp), the CPU-extension
-- query and the scalar math helpers it links (applications/_libs/cmp_math), and the three SIMD variants of the BC1
-- endpoint search, which upstream builds as the separate libraries CMP_Core_SSE, CMP_Core_AVX and CMP_Core_AVX512.
--
-- Consumers (Dependencies.lua UseCMPCore(); only Engine/Source/Engine/Asset/TextureCompression.cpp includes the header,
-- Scripts/ModuleRules.json) add cmp_core/source as an external include directory and include <cmp_core.h>. The library
-- has no configuration defines: CMP_CDECL and CMP_DEFAULTNULL, the only macros cmp_core.h reads, keep their defaults.
--
-- Floating point: the precise model without contraction, like JoltPhysics, MikkTSpace and every first-party project
-- (Architecture §2.2). Compressed texture blocks are cooked into the asset cache, whose keys do not include the build
-- configuration (§7.5), so a Debug and a Release editor must produce identical bytes; FMA contraction that depends on
-- the optimization level would break that. Scripts/CheckBuildConfig.py checks it. The SIMD variants are compiled, as
-- upstream compiles them, because the BC1 kernel references them, but the engine never enables them: its encoder calls
-- DisableSIMD() before its first block (Docs/Decisions/0034-m23-contract.md), so the result does not depend on the CPU.

project "CMP_Core"
	kind "StaticLib"
	language "C++"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Mirrors target_sources(CMP_Core ...) of cmp_core/CMakeLists.txt (the optional GTC and Brotli-G codecs are off
	-- upstream too), with core_simd.h and the three SIMD sources of CMP_Core_SSE, CMP_Core_AVX and CMP_Core_AVX512.
	files
	{
		"cmp_core/shaders/bc1_cmp.h",
		"cmp_core/shaders/bc1_common_kernel.h",
		"cmp_core/shaders/bc1_encode_kernel.h",
		"cmp_core/shaders/bc1_encode_kernel.cpp",
		"cmp_core/shaders/bc2_encode_kernel.h",
		"cmp_core/shaders/bc2_encode_kernel.cpp",
		"cmp_core/shaders/bc3_encode_kernel.h",
		"cmp_core/shaders/bc3_encode_kernel.cpp",
		"cmp_core/shaders/bc4_encode_kernel.h",
		"cmp_core/shaders/bc4_encode_kernel.cpp",
		"cmp_core/shaders/bc5_encode_kernel.h",
		"cmp_core/shaders/bc5_encode_kernel.cpp",
		"cmp_core/shaders/bc6_common_encoder.h",
		"cmp_core/shaders/bc6_encode_kernel.h",
		"cmp_core/shaders/bc6_encode_kernel.cpp",
		"cmp_core/shaders/bc7_cmpmsc.h",
		"cmp_core/shaders/bc7_common_encoder.h",
		"cmp_core/shaders/bc7_encode_kernel.h",
		"cmp_core/shaders/bc7_encode_kernel.cpp",
		"cmp_core/shaders/bcn_common_api.h",
		"cmp_core/shaders/bcn_common_kernel.h",
		"cmp_core/shaders/common_def.h",
		"cmp_core/source/cmp_core.h",
		"cmp_core/source/cmp_core.cpp",
		"cmp_core/source/cmp_math_func.h",
		"cmp_core/source/cmp_math_vec4.h",
		"cmp_core/source/core_simd.h",
		"applications/_libs/cmp_math/cmp_math_common.h",
		"applications/_libs/cmp_math/cmp_math_common.cpp",
		"applications/_libs/cmp_math/cpu_extensions.h",
		"applications/_libs/cmp_math/cpu_extensions.cpp",
	}

	-- target_include_directories(CMP_Core PUBLIC shaders source ../applications/_libs/cmp_math/).
	includedirs
	{
		"cmp_core/shaders",
		"cmp_core/source",
		"applications/_libs/cmp_math",
	}

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	-- Upstream: target_compile_definitions(CMP_Core PRIVATE _LINUX ASPM_GPU) on UNIX.
	filter "system:linux or macosx"
		defines { "_LINUX", "ASPM_GPU" }

	filter "system:linux"
		pic "On"

	-- The SIMD variants exist for x86-64 only (upstream compiles them on every UNIX and Windows build, which are x86-64).
	-- Each source gets the instruction set upstream gives its library: CMP_Core_SSE nothing on Windows and
	-- -march=nehalem (SSE4.2) elsewhere, CMP_Core_AVX /arch:AVX2 (-march=haswell), CMP_Core_AVX512 /arch:AVX-512
	-- (-march=knl); here the GCC and Clang spellings name the instruction sets themselves (-msse4.2; -mavx2 -mfma;
	-- -mavx512f -mavx512cd), since GCC 15 removed -march=knl, and MSVC's spelling is /arch:AVX512. clang-cl takes the AVX
	-- options in MSVC's spelling, with MSVC, and -msse4.2 for every source (below).
	filter "architecture:x86_64"
		files
		{
			"cmp_core/source/core_simd_sse.cpp",
			"cmp_core/source/core_simd_avx.cpp",
			"cmp_core/source/core_simd_avx512.cpp",
		}

	filter { "architecture:x86_64", "files:cmp_core/source/core_simd_avx.cpp", "toolset:msc* or clang", "action:vs*" }
		buildoptions { "/arch:AVX2" }

	filter { "architecture:x86_64", "files:cmp_core/source/core_simd_avx512.cpp", "toolset:msc* or clang", "action:vs*" }
		buildoptions { "/arch:AVX512" }

	filter { "architecture:x86_64", "files:cmp_core/source/core_simd_sse.cpp", "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-msse4.2" }

	filter { "architecture:x86_64", "files:cmp_core/source/core_simd_avx.cpp", "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-mavx2", "-mfma" }

	filter { "architecture:x86_64", "files:cmp_core/source/core_simd_avx512.cpp", "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-mavx512f", "-mavx512cd" }

	-- clang-cl (vs* with toolset clang, the portability build of Architecture §15.8): SSE4.2 for every source. Under _M_X64,
	-- which clang-cl defines as MSVC does, the kernels' shared cmp_floor (cmp_core/shaders/bcn_common_api.h) uses the SSE4.1
	-- intrinsic _mm_floor_ss, and core_simd_sse.cpp uses _mm_blendv_ps and _mm_floor_ps. MSVC compiles SSE4.1 intrinsics
	-- without an /arch option; Clang refuses them in a function without the sse4.1 target feature, as UseJoltPhysics
	-- records for Jolt's (Dependencies.lua). GCC and Clang outside Visual Studio keep upstream's per-file options (never
	-- compiled here, ADR 0028 decision 10). The engine needs SSE4.2 already (Jolt's /arch:SSE4.2), so this asks no more of
	-- the CPU.
	filter { "architecture:x86_64", "toolset:clang", "action:vs*" }
		buildoptions { "-msse4.2" }

	-- GCC and Clang: precise without contraction (premake's xcode4 generator adds -ffast-math for optimize "Full", which
	-- -fno-fast-math turns back off), as in Vendor/MikkTSpace/premake5.lua.
	filter { "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-fno-fast-math", "-ffp-contract=off" }

	-- clang-cl (the portability build of Architecture §15.8): the same options through /clang:.
	filter { "toolset:clang", "action:vs*" }
		buildoptions { "/clang:-fno-fast-math", "/clang:-ffp-contract=off" }

	-- Debug is optimized too, for size (/O1), with the Debug runtime (its CRT and iterator debugging, as every consumer
	-- links them): unoptimized, the BC6H encoder at CMP_Core's fixed quality made an environment import about six times
	-- slower (Docs/Decisions/0034-m23-contract.md decision 21 has the measurements). The three configurations still compile
	-- CMP_Core differently on purpose (Debug /O1, Release /O2, Dist "full" with link-time code generation): the cooked
	-- bytes must not depend on the configuration, and a read of uninitialized memory, such as the one that kept BC3 out
	-- (decision 13), leaves different stack contents under different code generation, so "TextureCompression: the fixture
	-- set compresses to the committed bytes in every configuration" can still expose one by comparing Debug and Release.
	-- Precise floating point without contraction (above) makes the level change no result of correct code. MSVC refuses
	-- its run-time checks (/RTC1, which a Debug-runtime project gets by default) with optimization, so they are off.
	filter "configurations:Debug"
		runtime "Debug"
		optimize "Size"
		runtimechecks "Off"
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
