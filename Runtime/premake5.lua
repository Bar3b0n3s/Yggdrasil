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

	-- The shipped executable names its PDB by file name only, never by the build machine's path: the symbols are archived in
	-- bin/Symbols and never shipped (§2.2, Docs/Decisions/0024-m15-contract.md decision 2). link.exe and lld-link both
	-- expand %_PDB%.
	filter { "configurations:Dist", "system:windows" }
		entrypoint "mainCRTStartup"
		linkoptions { "/PDBALTPATH:%_PDB%" }

	-- Linker map in every configuration, next to the executable: crash-address lookup, and the Dist symbol test of
	-- Architecture §14.4 (/MAP for link.exe and lld-link, -Wl,-Map for GNU ld and lld, -Wl,-map for ld64). The path is absolute, because make and ninja run the
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

	-- Exported Linux games look for shared libraries next to the executable first (§14.1, §16: RPATH $ORIGIN). The single
	-- quotes keep the shell from expanding it; make and ninja both turn $$ into $ (the portability stage checks the
	-- generated link lines, Docs/Decisions/0024-m15-contract.md decision 18).
	filter "system:linux"
		linkoptions { "-Wl,-rpath,'$$ORIGIN'" }

	filter {}
