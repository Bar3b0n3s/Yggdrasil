-- tinyexr (https://github.com/syoyo/tinyexr), vendored at v3.2.0 (commit 6f470c9ab24bf3992bc512ce07e8ecb00d9bf105). See
-- VENDOR.md. Upstream is a single header with an implementation section; tinyexr.cc is its implementation unit (it
-- defines TINYEXR_IMPLEMENTATION and includes tinyexr.h), compiled here, unmodified, as one static library. The version 3
-- C library under upstream's src/ and include/ is not vendored.
--
-- Consumers (Dependencies.lua UseTinyExr(); only Engine/Source/Engine/AssetPipeline/Importers/EnvironmentImporter.cpp
-- includes the header, Scripts/ModuleRules.json) define the same TINYEXR_ configuration macros as this project
-- (Scripts/CheckBuildConfig.py compares the family):
--   TINYEXR_USE_MINIZ=0, TINYEXR_USE_STB_ZLIB=1: ZIP and PXR24 inflate through stb_image's zlib decoder
--     (stbi_zlib_decode_buffer) and the unused writer deflates through stb_image_write's (stbi_zlib_compress), which
--     tinyexr declares itself with C linkage; their implementation is the Engine library's
--     (Graphics/ThirdParty/StbImageImplementation.cpp), so every executable that links this library links Engine too,
--     and no miniz is vendored (Docs/Decisions/0034-m23-contract.md decision 14);
--   TINYEXR_USE_THREAD=0, TINYEXR_USE_OPENMP=0: decoding runs on the calling thread only.
--
-- Floating point: the precise model without contraction, like every first-party project (Architecture §2.2): the decoded
-- texels are baked into a cooked environment, whose cache key does not include the build configuration (§7.5).

project "tinyexr"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"tinyexr.h",
		"tinyexr.cc",
		"exr_reader.hh",
		"streamreader.hh",
	}

	defines
	{
		"TINYEXR_USE_MINIZ=0",
		"TINYEXR_USE_STB_ZLIB=1",
		"TINYEXR_USE_THREAD=0",
		"TINYEXR_USE_OPENMP=0",
	}

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (premake's xcode4 generator adds -ffast-math for optimize "Full", which
	-- -fno-fast-math turns back off), as in Vendor/CMP_Core/premake5.lua.
	filter { "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-fno-fast-math", "-ffp-contract=off" }

	-- clang-cl (the portability build of Architecture §15.8): the same options through /clang:.
	filter { "toolset:clang", "action:vs*" }
		buildoptions { "/clang:-fno-fast-math", "/clang:-ffp-contract=off" }

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
