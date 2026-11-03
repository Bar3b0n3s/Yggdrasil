-- Tests: the doctest unit-test executable (Docs/Architecture.md §2.2, §15). It may include everything, so it links
-- EditorCore and Engine. It does not exist in Dist (see Editor/premake5.lua for why removeconfigurations is used).

project "Tests"
	kind "ConsoleApp"
	ApplyFirstPartySettings()
	removeconfigurations { "Dist" }

	-- TestMain.cpp includes TestsPCH.h first and creates the precompiled header on MSVC. xcode4 needs the absolute
	-- path (see Engine/premake5.lua).
	pchheader "TestsPCH.h"
	pchsource "Source/TestMain.cpp"

	filter "action:xcode4"
		pchheader (RepositoryRoot .. "/Tests/Source/TestsPCH.h")

	filter {}

	files
	{
		"Source/**.h",
		"Source/**.cpp",
		"../Editor/Source/Editor/**.cpp",
		"../Runtime/Source/Runtime/RuntimeCursor.cpp"
	}

	-- Exercise the production UI with a headless ImGui context, without linking the Editor executable or moving UI
	-- into EditorCore. App/process integration is tested through the real executable; it has its own entry point.
	removefiles
	{
		"../Editor/Source/Editor/EditorMain.cpp",
		"../Editor/Source/Editor/EditorApp.cpp"
	}

	filter "files:../Editor/Source/Editor/**.cpp"
		enablepch "Off"

	-- The Runtime's pure cursor mapping (RuntimeCursor.cpp, a RuntimeCore source without a precompiled header), compiled here
	-- as the editor's sources are, so its test runs without a Runtime window (Docs/Decisions/0032-m21-contract.md, W21.1's
	-- review).
	filter "files:../Runtime/Source/Runtime/RuntimeCursor.cpp"
		enablepch "Off"

	filter {}

	includedirs
	{
		"Source",
		RepositoryRoot .. "/Editor/Source",
		RepositoryRoot .. "/Runtime/Source"
	}

	externalincludedirs
	{
		IncludeDir.doctest,
		IncludeDir.cgltf,
		IncludeDir.stb
	}

	links
	{
		"EditorCore",
		"Engine"
	}

	-- Support/PlatformProbes.cpp reads an exported executable's version resource back with the OS's own parser
	-- (GetFileVersionInfoW, VerQueryValueW), the independent oracle of the resource patcher's acceptance test
	-- (Docs/Decisions/0024-m15-contract.md decision 4).
	filter "system:windows"
		links { "version" }

	filter {}

	-- Tests spawn the Editor and Runtime executables of the same configuration (Test::GetBuiltExecutablePath), so building
	-- Tests builds them too.
	dependson
	{
		"Editor",
		"Runtime"
	}

	UseImGui()
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseMikkTSpace()
	UseNativeFileDialog()
	UseLuau(true)
