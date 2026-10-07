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
		"Source/**.cpp"
	}

	includedirs
	{
		"Source",
		RepositoryRoot .. "/Editor/Source"
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
	UseLuau(true)
