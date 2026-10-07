-- Runtime: the executable that runs exported games (Docs/Architecture.md §2.2, §14.3). It links Engine without the
-- editor libraries and uses no precompiled header.

project "Runtime"
	kind "ConsoleApp"
	ApplyFirstPartySettings()

	files
	{
		"Source/Runtime/**.h",
		"Source/Runtime/**.cpp"
	}

	includedirs
	{
		"Source"
	}

	links
	{
		"Engine"
	}

	UseImGui()
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseMikkTSpace()
	UseLuau(false)

	-- Shipping builds have no console window. The C++ main() stays the entry point on Windows.
	filter "configurations:Dist"
		kind "WindowedApp"

	filter { "configurations:Dist", "system:windows" }
		entrypoint "mainCRTStartup"

	-- Linker map in every configuration, next to the executable: crash-address lookup, and the Dist symbol test of
	-- Architecture §14.4 ("/MAP on MSVC, -Wl,-Map elsewhere"; link.exe and lld-link both take /MAP). The path is absolute, because make and ninja run the
	-- linker from different directories.
	filter "system:windows"
		linkoptions { "/MAP" }

	filter "system:linux"
		linkoptions { "-Wl,-Map=" .. WorkspaceLocation .. "/bin/" .. OutputDir .. "/%{prj.name}/%{prj.name}.map" }

	filter "system:macosx"
		linkoptions { "-Wl,-map," .. WorkspaceLocation .. "/bin/" .. OutputDir .. "/%{prj.name}/%{prj.name}.map" }

	-- Exported macOS games bundle the Vulkan loader and ICD in Contents/Frameworks (§16).
	filter "system:macosx"
		linkoptions { "-Wl,-rpath,@executable_path/../Frameworks" }

	filter {}
