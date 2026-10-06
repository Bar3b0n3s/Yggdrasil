-- Root premake script (Docs/Architecture.md §2.2).
--   Windows: Vendor/premake/bin/premake5.exe vs2026
--   Linux:   premake5 gmake   (or ninja)
--   macOS:   premake5 xcode4
-- Scripts/Generate.py wraps these. --to=<dir> writes the generated files to <dir> instead of next to each script.
--   All:     premake5 compile-commands   (compile_commands.json for clang-tidy and Scripts/Lint.py)

-- The custom compile-commands action (Docs/Architecture.md §2.2). Actions must be registered before the workspace.
include "Scripts/Premake/CompileCommands.lua"

newoption
{
	trigger = "to",
	value = "path",
	description = "Write the generated workspace and project files to this directory"
}

include "Dependencies.lua"

-- The workspace name, defined here only (Architecture: the display name appears in the workspace name,
-- ENGINE_PRODUCT_NAME and the docs). The generated .slnx and .xcworkspace are named after it, and Scripts/Lib/paths.py
-- reads it from this line. Scripts that re-open the workspace (Tests/Data/BuildConfig) use the variable.
WorkspaceName = "Yggdrasil"

-- Absolute directory of the generated workspace file (and of bin/ and bin-int/): the repository root, or --to
-- (relative to the repository root).
WorkspaceLocation = path.getabsolute(_OPTIONS["to"] or ".", RepositoryRoot)

workspace (WorkspaceName)
	configurations { "Debug", "Release", "Dist" }
	startproject "Editor"

	-- Without --to every project is generated next to its own premake5.lua; with it, everything goes to one directory.
	if _OPTIONS["to"] then
		location (WorkspaceLocation)
	end

	filter "system:macosx"
		architecture "ARM64"
		-- Minimum macOS version (§16) for every project, vendored ones included, so that the linker never mixes
		-- objects built for different deployment targets.
		systemversion "14.0"

	-- xcode4 applies MACOSX_DEPLOYMENT_TARGET to compiling and linking; the make-style generators pass systemversion
	-- to the compiler only, so the linker gets the minimum version explicitly.
	filter { "system:macosx", "action:not xcode4" }
		linkoptions { "-mmacosx-version-min=14.0" }

	filter "system:not macosx"
		architecture "x86_64"

	-- Workspace scope only (ODR): NDEBUG changes the layout of vulkan.hpp's dispatcher, so NVRHI and every consumer
	-- must agree on it (Vendor/NVRHI/VENDOR.md). Never define NDEBUG in a project script.
	filter "configurations:Dist"
		defines { "NDEBUG" }

	filter {}

OutputDir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"

group "Dependencies"
	include "Vendor/GLFW"
	include "Vendor/NVRHI"
	include "Vendor/imgui"
	include "Vendor/ImGuizmo"
	include "Vendor/Luau"
	include "Vendor/JoltPhysics"
	include "Vendor/miniaudio"
	include "Vendor/spdlog"

group "Engine"
	include "Engine"

group "Editor"
	include "Editor"

group ""
	include "Runtime"
	include "Tests"
