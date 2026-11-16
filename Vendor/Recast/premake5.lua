-- Recast (https://github.com/recastnavigation/recastnavigation), vendored at v1.6.0 (commit
-- 6dc1667f580357e8a2154c28b7867bea7e8ad3a7). See VENDOR.md. The navigation-mesh builder of the editor's navigation.bake
-- (M30, Docs/Decisions/0045-m30-navigation.md): voxelizes the bake's triangles and builds the polygon and detail meshes
-- that the NavMesh asset stores. Upstream's Include/ and Source/ unmodified, as one static library.
--
-- Consumers (Dependencies.lua UseRecast(); only Editor/Source/EditorCore/Navigation/NavMeshBuilder.cpp includes its
-- headers, Scripts/ModuleRules.json) define none of the RC_ family: Recast reads no configuration macro in its headers, and
-- RC_MAX_LAYERS_DEF and RC_MAX_NEIS_DEF, which only RecastLayers.cpp reads, keep their defaults. The editor's bake only:
-- EditorCore, Editor and Tests call UseRecast(), never the Runtime's projects (UseRuntimeDependencies).
--
-- Floating point: the precise model without contraction, like every first-party project (Architecture §2.2): two bakes of
-- one scene write byte-identical files in every configuration of one compiler.

project "Recast"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"Include/*.h",
		"Source/*.cpp",
	}

	includedirs { "Include" }

	-- MSVC: /fp:precise (premake "Default"), which never contracts into FMA.
	floatingpoint "Default"

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

	-- GCC and Clang: precise without contraction (premake's xcode4 generator adds -ffast-math for optimize "Full", which
	-- -fno-fast-math turns back off), as in Vendor/tinyexr/premake5.lua.
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
