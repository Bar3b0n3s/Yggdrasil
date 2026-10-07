-- Engine static library and the Shaders utility project (Docs/Architecture.md §2.2, §8.12).

local ShaderSourceDirectory = RepositoryRoot .. "/Resources/Shaders"
-- Shaders compile into ShaderOutputDirectory (premake5.lua), the repository's bin/<OutputDir>/Shaders, which development
-- builds mount as shaders:// through ENGINE_SHADER_DIRECTORY (Dependencies.lua; Architecture §2.2, §8.12;
-- Docs/Decisions/0009-m5-decisions.md decision 4).

-- The single custom build rule that compiles every shader. Its inputs are Shaders.json, every .slang and .h file
-- under Resources/Shaders, the compiler script and the pinned toolchain, so an IDE build re-runs it when only a
-- shader changed. A shader file added later is tracked once the projects are regenerated; until then the script's
-- own depfile check still recompiles every program that imports it. CompileShaders.py writes the stamp last, and
-- only after every program compiled and validated.
local function AddShaderCompileRule()
	files { ShaderSourceDirectory .. "/Shaders.json" }

	local compile = "\"" .. RepositoryRoot .. "/Scripts/CompileShaders.py\" --config %{cfg.buildcfg} --output-dir \"" .. ShaderOutputDirectory .. "\""

	filter "files:**/Resources/Shaders/Shaders.json"
		buildmessage "Compiling shaders (%{cfg.buildcfg})"
		buildinputs
		{
			os.matchfiles(ShaderSourceDirectory .. "/**.slang"),
			os.matchfiles(ShaderSourceDirectory .. "/**.h"),
			RepositoryRoot .. "/Scripts/CompileShaders.py",
			RepositoryRoot .. "/Scripts/Lib/Toolchain.json"
		}
		buildoutputs { ShaderOutputDirectory .. "/.stamp" }

	-- The interpreter Scripts/Generate.py runs under (--python), so build steps get the same verified Python 3.10+
	-- everywhere. This matters in Xcode script phases, whose PATH finds Xcode's own older python3 first.
	if _OPTIONS["python"] then
		filter "files:**/Resources/Shaders/Shaders.json"
			buildcommands { "\"" .. _OPTIONS["python"] .. "\" " .. compile }
	else
		filter { "files:**/Resources/Shaders/Shaders.json", "system:windows" }
			buildcommands { "python " .. compile }

		-- Linux and macOS ship python3 without a "python" alias.
		filter { "files:**/Resources/Shaders/Shaders.json", "system:not windows" }
			buildcommands { "python3 " .. compile }
	end

	filter {}
end

-- premake's ninja generator supports neither Utility projects nor dependson edges to them. For ninja the rule is
-- attached to Engine instead (below), so every Engine object depends on the shader stamp.
local UseShadersProject = _ACTION ~= "ninja"

if UseShadersProject then
	project "Shaders"
		kind "Utility"

		targetdir (ShaderOutputDirectory)
		objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

		files
		{
			ShaderSourceDirectory .. "/**.slang",
			ShaderSourceDirectory .. "/**.h"
		}

		AddShaderCompileRule()
end

project "Engine"
	kind "StaticLib"
	ApplyFirstPartySettings()

	-- EnginePCH.cpp creates the precompiled header on MSVC; every Engine .cpp file includes EnginePCH.h first.
	pchheader "EnginePCH.h"
	pchsource "Source/EnginePCH.cpp"

	-- Xcode resolves a relative GCC_PREFIX_HEADER against the directory of the .xcodeproj, not the include path, so
	-- xcode4 gets the absolute path (valid in place and with --to). The other generators keep the include-relative
	-- spelling MSVC needs.
	filter "action:xcode4"
		pchheader (RepositoryRoot .. "/Engine/Source/EnginePCH.h")

	filter {}

	files
	{
		"Source/**.h",
		"Source/**.cpp"
	}

	-- Implementation translation units of vendored code (Architecture §2.2): each defines a library's implementation
	-- macro, or includes a vendored .cpp, before anything else, so it is compiled without the precompiled header.
	filter "files:**/Engine/*/ThirdParty/**.cpp or files:**/Engine/ImGui/ImGuiGlfwImplementation.cpp"
		enablepch "Off"

	filter {}

	externalincludedirs
	{
		IncludeDir.cgltf,
		IncludeDir.stb
	}

	UseImGui()
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseLuau(false)

	if UseShadersProject then
		dependson { "Shaders" }
	else
		AddShaderCompileRule()
	end

	-- Platform code lives in Platform/<OS>/ (Appendix A) and is compiled only for its own target.
	filter "system:windows"
		removefiles { "Source/Engine/Platform/Linux/**", "Source/Engine/Platform/MacOS/**", "Source/Engine/Platform/Posix/**" }

	filter "system:linux"
		removefiles { "Source/Engine/Platform/Windows/**", "Source/Engine/Platform/MacOS/**" }

	filter "system:macosx"
		removefiles { "Source/Engine/Platform/Windows/**", "Source/Engine/Platform/Linux/**" }

	filter {}
