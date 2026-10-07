-- EditorCore (UI-free editor library, unit-tested by Tests) and the Editor executable (Docs/Architecture.md §2.2,
-- §12.1). Neither exists in Dist: removeconfigurations drops the configuration and marks the projects Build=false
-- for Dist in the generated solution (a project-scoped `configurations` list would not).

project "EditorCore"
	kind "StaticLib"
	ApplyFirstPartySettings()
	removeconfigurations { "Dist" }

	-- EditorPCH.h is compiled separately by EditorCore and Editor; EditorPCH.cpp creates it on MSVC. xcode4 needs the
	-- absolute path (see Engine/premake5.lua).
	pchheader "EditorPCH.h"
	pchsource "Source/EditorPCH.cpp"

	filter "action:xcode4"
		pchheader (RepositoryRoot .. "/Editor/Source/EditorPCH.h")

	filter {}

	files
	{
		"Source/EditorPCH.h",
		"Source/EditorPCH.cpp",
		"Source/EditorCore/**.h",
		"Source/EditorCore/**.cpp"
	}

	includedirs
	{
		"Source"
	}

	externalincludedirs
	{
		IncludeDir.cgltf,
		IncludeDir.stb
	}

	-- UI-free (Architecture §3): no UseImGui(). The other libraries supply the defines and include paths the Engine
	-- headers it includes need; LuauAnalysis is for the script type checker.
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseMikkTSpace()
	UseLuau(true)

project "Editor"
	kind "ConsoleApp"
	ApplyFirstPartySettings()
	removeconfigurations { "Dist" }

	pchheader "EditorPCH.h"
	pchsource "Source/EditorPCH.cpp"

	filter "action:xcode4"
		pchheader (RepositoryRoot .. "/Editor/Source/EditorPCH.h")

	filter {}

	files
	{
		"Source/EditorPCH.h",
		"Source/EditorPCH.cpp",
		"Source/Editor/**.h",
		"Source/Editor/**.cpp"
	}

	includedirs
	{
		"Source"
	}

	externalincludedirs
	{
		IncludeDir.cgltf,
		IncludeDir.stb
	}

	links
	{
		"EditorCore",
		"Engine"
	}

	UseImGui()
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseMikkTSpace()
	UseLuau(true)
