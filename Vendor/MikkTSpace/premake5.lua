-- MikkTSpace (https://github.com/mmikk/MikkTSpace), vendored at commit 3e895b49d05ea07e4c2133156cfa94369e19e409. See
-- VENDOR.md. Upstream ships only mikktspace.c and mikktspace.h and no build file; this project compiles the C file as a
-- static library, unmodified.
--
-- Consumers (Dependencies.lua UseMikkTSpace(), only the glTF importer's tangent generator includes it,
-- Scripts/ModuleRules.json) add the include directory as an external include directory. The library has no
-- configuration defines.
--
-- Floating point: the precise model without contraction, like JoltPhysics and every first-party project (Architecture
-- §2.2). Generated tangents are part of cooked meshes, which must be identical in every configuration of one toolchain
-- (Architecture §7.4, §7.5: cache keys do not include the configuration), and FMA contraction would differ between
-- optimization levels. Scripts/CheckBuildConfig.py checks it.

project "MikkTSpace"
	kind "StaticLib"
	language "C"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"mikktspace.h",
		"mikktspace.c",
	}

	includedirs
	{
		".",
	}

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (GCC's C default is -ffp-contract=fast; premake's xcode4 generator adds
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
