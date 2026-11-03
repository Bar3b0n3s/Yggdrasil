-- SheenBidi (https://github.com/Tehreer/SheenBidi), vendored at release v2.9.0 (commit
-- 83f77108a2873600283f6da4b326a2dca7a3a7a6). See VENDOR.md. The Unicode bidirectional algorithm (UAX #9) and its script
-- itemizer, in C. Every source of Source/ is compiled on its own, as upstream's CMake build does without SB_CONFIG_UNITY,
-- so no configuration macro is defined (SB_CONFIG_UNITY would also change the public SBConfig.h's SB_INTERNAL, which
-- consumers would have to match); Source/SheenBidi.c is the unity build's file and holds nothing else, so it is left
-- out. Upstream's sources are unmodified.
--
-- Consumers (Dependencies.lua UseSheenBidi(); the text-measurement layer's bidirectional runs,
-- Docs/Decisions/0033-m22-contract.md, C22.3) add Headers/ as an external include directory and include
-- <SheenBidi/SheenBidi.h>.
--
-- Floating point: the precise model without contraction, like every first-party project (Architecture §2.2): the
-- library's results reach the UI's layout, which must be identical in every configuration. Scripts/CheckBuildConfig.py
-- checks it (PRECISE_VENDOR_PROJECTS, the FastMathSheenBidi fixture).

project "SheenBidi"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"Headers/SheenBidi/*.h",
		"Source/*.h",
		"Source/*.c",
	}

	removefiles
	{
		"Source/SheenBidi.c",
	}

	includedirs
	{
		"Headers",
		"Source",
	}

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (GCC's C default is -ffp-contract=fast; premake's xcode4 generator adds
	-- -ffast-math for optimize "Full", which -fno-fast-math turns back off), as in Vendor/MikkTSpace/premake5.lua.
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
