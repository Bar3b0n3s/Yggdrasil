-- HarfBuzz (https://github.com/harfbuzz/harfbuzz), vendored at release 15.0.0 (commit
-- 35f656a1a2bab548ea7f54a6dd3fba2b09680d14). See VENDOR.md. The library is built the way upstream's own amalgamated
-- build compiles it: one translation unit, src/harfbuzz-subset.cc, which includes every source of the shaping library
-- (the OpenType shaper, the built-in Unicode data hb-ucd and the font, face and buffer objects) and of the subsetter
-- (hb-subset), and none of the platform backends (FreeType, ICU, GLib, CoreText, DirectWrite, GDI, Uniscribe,
-- Graphite2, WebAssembly). Upstream's sources are unmodified.
--
-- Consumers (Dependencies.lua UseHarfBuzz(); the text-measurement layer's shaping and the font importer's shaping glyphs
-- and subset, Docs/Decisions/0033-m22-contract.md, C22.3) add src/ as an external include directory and include <hb.h>,
-- <hb-ot.h> and <hb-subset.h>. No public header reads a configuration macro this project defines.
--
-- Defines, both private to the library's own translation unit (Scripts/CheckBuildConfig.py's VENDOR_PRIVATE_DEFINES):
-- HB_NO_GETENV removes upstream's reads of the HB_OPTIONS, HB_SHAPER_LIST, HB_FACE_LOADER and HB_FONT_FUNCS environment
-- variables, and HB_NO_SETLOCALE its read of the C locale (hb_language_get_default), so nothing in the process's
-- environment changes how text is shaped (Roadmap M22: shaped text is byte-identical across configurations and runs).
--
-- Floating point: the precise model without contraction, like JoltPhysics, MikkTSpace, Luau and every first-party
-- project (Architecture §2.2): shaped glyph positions reach the UI's layout and the cooked fonts, which must be identical
-- in every configuration. Scripts/CheckBuildConfig.py checks it (PRECISE_VENDOR_PROJECTS, the FastMathHarfBuzz fixture).

project "HarfBuzz"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- The amalgamated translation unit is the only compiled file; the headers are listed for the IDE. The .cc files it
	-- includes are kept beside it and never compiled on their own.
	files
	{
		"src/harfbuzz-subset.cc",
		"src/**.h",
		"src/**.hh",
	}

	includedirs
	{
		"src",
	}

	defines
	{
		"HB_NO_GETENV",
		"HB_NO_SETLOCALE",
	}

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	-- Upstream's MSVC options (its CMakeLists.txt and meson.build): /bigobj, since the amalgamated subsetter exceeds the
	-- object format's default section limit (C1128), and /utf-8 for the sources' UTF-8 comments.
	filter { "system:windows", "action:vs*" }
		buildoptions { "/bigobj", "/utf-8" }

	-- Upstream's Windows definitions too (its CMakeLists.txt): _CRT_SECURE_NO_WARNINGS and _CRT_NONSTDC_NO_WARNINGS, so
	-- the CRT does not mark the C functions the sources call (strncpy, strcpy, fopen, mbstowcs) deprecated (C4996). They
	-- change no declaration's layout and only this project's translation unit sees them.
	filter "system:windows"
		defines { "_CRT_SECURE_NO_WARNINGS", "_CRT_NONSTDC_NO_WARNINGS" }

	-- Upstream's MSVC options too: its own build disables C4244 and C4267 (lossy integer and floating-point conversions,
	-- "e.g. double/size_t -> int"), which its sources make on purpose at MSVC's default level (about a hundred places in
	-- the vendored translation unit). The sources stay unmodified, so they are disabled for this vendored project only, as
	-- Vendor/miniaudio/premake5.lua disables its one upstream C4244; consumers' translation units keep every warning.
	filter { "toolset:msc*" }
		disablewarnings { "4244", "4267" }

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (GCC's default is -ffp-contract=fast; premake's xcode4 generator adds
	-- -ffast-math for optimize "Full", which -fno-fast-math turns back off), as in Vendor/JoltPhysics/premake5.lua.
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
