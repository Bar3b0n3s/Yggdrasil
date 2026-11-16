-- Detour (https://github.com/recastnavigation/recastnavigation), vendored at v1.6.0 (commit
-- 6dc1667f580357e8a2154c28b7867bea7e8ad3a7). See VENDOR.md. The run-time navigation mesh and its queries (M30,
-- Docs/Decisions/0045-m30-navigation.md): the Navigation module builds a dtNavMesh from a NavMesh asset's validated payload
-- with dtCreateNavMeshData and answers path, closest-point and surface-ray queries in the fixed step. Upstream's Include/
-- and Source/ unmodified, as one static library.
--
-- Consumers (Dependencies.lua UseDetour(); only the Navigation module's private files include its headers,
-- Scripts/ModuleRules.json) define none of the DT_ family: DT_POLYREF64 (64-bit polygon references) and
-- DT_VIRTUAL_QUERYFILTER (a virtual dtQueryFilter) change the layout of dtPolyRef and dtQueryFilter, so the library and
-- every consumer leave both undefined. Every executable links it: the Runtime's sessions answer navigation queries.
--
-- Floating point: the precise model without contraction, like every first-party project (Architecture §2.2): queries run
-- on the simulation path, so a path is identical in Debug, Release and Dist.

project "Detour"
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
