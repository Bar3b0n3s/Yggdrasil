-- meshoptimizer (https://github.com/zeux/meshoptimizer), vendored at v1.3 (commit
-- 9e1f07b159d3cb777f1c67ed31fc11fd117986f4). See VENDOR.md. The glTF importer's generated mesh levels (M46,
-- Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md): meshopt_simplify over a submesh's positions. Upstream's
-- src/ unmodified, as one static library.
--
-- Consumers (Dependencies.lua UseMeshOptimizer(); only AssetPipeline/Private/MeshSimplifier.cpp includes the header,
-- Scripts/ModuleRules.json) define none of the MESHOPTIMIZER_ family. AssetPipeline is editor-only: EditorCore, Editor
-- and Tests link the library, the Runtime's projects never do (UseRuntimeDependencies).
--
-- Floating point: the precise model without contraction, like MikkTSpace and every first-party project (Architecture
-- §2.2). Generated levels are cooked into meshes, which must be identical from a Debug and a Release editor (cache keys
-- do not include the configuration, Architecture §7.5), and FMA contraction would differ between optimization levels.
-- Scripts/CheckBuildConfig.py checks it.

project "meshoptimizer"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"src/*.h",
		"src/*.cpp",
	}

	includedirs { "src" }

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (premake's xcode4 generator adds -ffast-math for optimize "Full", which
	-- -fno-fast-math turns back off), as in Vendor/Detour/premake5.lua.
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
