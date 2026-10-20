-- Dependencies.lua: include directories, the settings every workspace applies (OutputDir, ApplyWorkspaceSettings()),
-- first-party project settings, one Use<Lib>() function per vendored library and the Runtime executable's settings
-- (UseRuntimeDependencies(), ApplyRuntimeExecutableSettings()) (Docs/Architecture.md §2.2).
--
-- The defines in every Use<Lib>() are copied verbatim from Vendor/<Lib>/VENDOR.md and must stay identical to what
-- Vendor/<Lib>/premake5.lua defines for the library itself. A mismatch is an ODR/ABI violation (silent memory
-- corruption), not a link error. Scripts/CheckBuildConfig.py verifies the generated projects.
--
-- Every Use<Lib>() applies the compile settings (external include directories, defines, flags) to the calling
-- project. Libraries are only linked into projects that produce a binary (ConsoleApp, WindowedApp, SharedLib):
-- a static library never links another static library, so the executables list every library they need. Call the
-- Use<Lib>() functions after `links { "EditorCore", "Engine" }` so that GNU ld sees dependents before dependencies.

-- Absolute path of the repository root. premake writes paths relative to each generated project, so this also works
-- when the projects are generated elsewhere with --to.
RepositoryRoot = path.getdirectory(_SCRIPT)

OutputDir = "%{cfg.buildcfg}-%{cfg.system}-%{cfg.architecture}"
-- The compiled SPIR-V of a configuration: the Shaders project's output (Engine/premake5.lua) and the directory
-- development builds mount as shaders:// (ENGINE_SHADER_DIRECTORY, below). Always in the repository's bin/, also for a
-- workspace generated elsewhere with --to, so every workspace of this checkout shares one shader output per
-- configuration; the SPIR-V does not depend on the C++ toolset (Docs/Decisions/0009-m5-decisions.md decision 4).
ShaderOutputDirectory = RepositoryRoot .. "/bin/" .. OutputDir .. "/Shaders"

-- Settings of every workspace the repository generates, at workspace scope: premake5.lua's, and the one
-- Scripts/Premake/NativeModule.lua generates for a game's native module (Docs/Decisions/0030-m19-contract.md decision
-- 30), whose code links with the engine's libraries and so must agree with them on the target and NDEBUG.
function ApplyWorkspaceSettings()
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

	filter "system:windows"
		-- The test executable exceeds the 32-bit linker's address space as subsystems accumulate.
		preferredtoolarchitecture "x86_64"

	-- Workspace scope only (ODR): NDEBUG changes the layout of vulkan.hpp's dispatcher, so NVRHI and every consumer
	-- must agree on it (Vendor/NVRHI/VENDOR.md). Never define NDEBUG in a project script.
	filter "configurations:Dist"
		defines { "NDEBUG" }

	filter {}
end

IncludeDir = {}
IncludeDir["GLFW"] = RepositoryRoot .. "/Vendor/GLFW/include"
IncludeDir["NVRHI"] = RepositoryRoot .. "/Vendor/NVRHI/include"
IncludeDir["VulkanHeaders"] = RepositoryRoot .. "/Vendor/NVRHI/thirdparty/Vulkan-Headers/include"
IncludeDir["JoltPhysics"] = RepositoryRoot .. "/Vendor/JoltPhysics"
IncludeDir["spdlog"] = RepositoryRoot .. "/Vendor/spdlog/include"
IncludeDir["miniaudio"] = RepositoryRoot .. "/Vendor/miniaudio"
IncludeDir["Luau"] =
{
	RepositoryRoot .. "/Vendor/Luau/Common/include",
	RepositoryRoot .. "/Vendor/Luau/Ast/include",
	RepositoryRoot .. "/Vendor/Luau/Bytecode/include",
	RepositoryRoot .. "/Vendor/Luau/Compiler/include",
	RepositoryRoot .. "/Vendor/Luau/Config/include",
	RepositoryRoot .. "/Vendor/Luau/VM/include",
	RepositoryRoot .. "/Vendor/Luau/Require/include"
}
IncludeDir["LuauAnalysis"] = RepositoryRoot .. "/Vendor/Luau/Analysis/include"
IncludeDir["ImGui"] = RepositoryRoot .. "/Vendor/imgui"
IncludeDir["ImGuizmo"] = RepositoryRoot .. "/Vendor/ImGuizmo/src"
IncludeDir["glm"] = RepositoryRoot .. "/Vendor/glm"
IncludeDir["entt"] = RepositoryRoot .. "/Vendor/entt/src"
IncludeDir["json"] = RepositoryRoot .. "/Vendor/json/single_include"
IncludeDir["cgltf"] = RepositoryRoot .. "/Vendor/cgltf"
IncludeDir["stb"] = RepositoryRoot .. "/Vendor/stb"
IncludeDir["MikkTSpace"] = RepositoryRoot .. "/Vendor/MikkTSpace"
IncludeDir["doctest"] = RepositoryRoot .. "/Vendor/doctest"

-- Project kinds that produce a linked binary and therefore link libraries.
local LinkingKinds = "kind:ConsoleApp or WindowedApp or SharedLib"

-- Settings shared by Engine, EditorCore, Editor, Runtime and Tests.
function ApplyFirstPartySettings()
	language "C++"
	cppdialect "C++23"
	staticruntime "off"
	exceptionhandling "On"
	rtti "Default"
	warnings "Extra"
	fatalwarnings { "All" }
	externalwarnings "Off"
	multiprocessorcompile "On"

	-- Precise floating point (§2.2, §9.1). "Default" is /fp:precise on MSVC, which never contracts into FMA.
	-- premake's "Precise" is not used: for Clang it emits -ffp-model=precise, which re-enables -ffp-contract=on.
	floatingpoint "Default"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	-- Engine/Config holds EnTT's user configuration header <entt/ext/config.h>, which routes ENTT_ASSERT to
	-- ENGINE_CORE_ASSERT (Architecture §4.5). EnTT finds it through __has_include, so it must be on the include path of
	-- every project that may include EnTT.
	includedirs
	{
		RepositoryRoot .. "/Engine/Source",
		RepositoryRoot .. "/Engine/Config",
		RepositoryRoot .. "/Resources/Shaders"
	}

	externalincludedirs
	{
		IncludeDir.glm,
		IncludeDir.entt,
		IncludeDir.json
	}

	-- First-party only: no vendored library includes these headers with different settings.
	defines
	{
		"GLM_FORCE_DEPTH_ZERO_TO_ONE",
		"GLM_ENABLE_EXPERIMENTAL",
		"GLM_FORCE_CTOR_INIT",
		"JSON_USE_IMPLICIT_CONVERSIONS=0"
	}

	filter "system:windows"
		systemversion "latest"
		defines { "ENGINE_PLATFORM_WINDOWS", "WIN32_LEAN_AND_MEAN" }

	filter "system:linux"
		pic "On"
		defines { "ENGINE_PLATFORM_LINUX" }

	filter "system:macosx"
		defines { "ENGINE_PLATFORM_MACOS" }

	filter "toolset:msc*"
		conformancemode "On"
		usestandardpreprocessor "On"
		buildoptions { "/utf-8", "/Zc:__cplusplus" }

	-- GCC and Clang (gmake, ninja, xcode4 and the compile-commands database): precise model without contraction on
	-- every architecture (GCC's C++ default is -ffp-contract=fast). -fno-fast-math resets the -ffast-math that
	-- premake's xcode4 generator adds for optimize "Full" (Dist). Clang's own default is the precise model, so this is
	-- -ffp-model=precise without FMA contraction. Spelling it "-ffp-model=precise -ffp-contract=off" makes current
	-- Clang (verified: 22.1.3) report -Woverriding-option, an error under -Werror.
	filter { "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-Wshadow", "-fno-fast-math", "-ffp-contract=off" }

	-- clang-cl (vs2026 with toolset "clang", the portability build of §15.8): the same flags. clang-cl takes GCC-style
	-- -f options only through /clang:. The msc* settings above do not apply: clang is standard-conforming without
	-- them, compiles UTF-8 sources by default and rejects /Zc:preprocessor under -Werror.
	filter { "toolset:clang", "action:vs*" }
		buildoptions { "-Wshadow", "/clang:-fno-fast-math", "/clang:-ffp-contract=off" }

	-- Clang on Linux links with lld, as the CI Clang job does: lld links the Dist LTO bitcode natively, where the
	-- system linker would need the LLVM gold plugin. Scripts/Lib/toolchain.py requires it for the clang toolset.
	filter { "toolset:clang", "system:linux" }
		linkoptions { "-fuse-ld=lld" }

	-- Every Windows executable embeds the application manifest (PerMonitorV2 DPI, UTF-8 code page, long paths).
	filter { "system:windows", "kind:ConsoleApp or WindowedApp" }
		files { RepositoryRoot .. "/Engine/Source/Engine/Platform/Windows/App.manifest" }

	-- The display name for the executables (Architecture: it appears only in the workspace name, ENGINE_PRODUCT_NAME and
	-- the docs): the Editor's window title and the user-data folder of the Editor and Tests (§4.4). The Engine and
	-- EditorCore libraries never see it, so engine code cannot put an exported game's files into the engine's folder
	-- (Docs/Decisions/0005-m2-decisions.md).
	filter "kind:ConsoleApp or WindowedApp"
		defines { "ENGINE_PRODUCT_NAME=\"" .. WorkspaceName .. "\"" }

	-- The Windows system libraries behind Engine/Source/Engine/Platform/Windows: dbghelp (MiniDumpWriteDump and stack
	-- symbols for crash reports), ws2_32 (sockets), bcrypt (BCryptGenRandom), shell32 and ole32 (SHGetKnownFolderPath and
	-- CoTaskMemFree). Every executable links Engine, so every executable links them.
	filter { "system:windows", "kind:ConsoleApp or WindowedApp" }
		links { "dbghelp", "ws2_32", "bcrypt", "shell32", "ole32" }

	-- Luau analysis recursion needs more than the default 1 MB stack with unoptimized MSVC frames.
	filter { "system:windows", "configurations:Debug", "kind:ConsoleApp or WindowedApp" }
		linkoptions { "/STACK:2097152" }

	-- AddressSanitizer's stack redzones enlarge the Release frames of the hardening stage's workspace the same way
	-- (premake5.lua --sanitize=address; Docs/Decisions/0024-m15-contract.md decision 12, risk 12). Without the option nothing
	-- changes.
	filter { "system:windows", "configurations:Release", "options:sanitize=address", "kind:ConsoleApp or WindowedApp" }
		linkoptions { "/STACK:2097152" }

	filter "configurations:Debug"
		defines { "ENGINE_DEBUG" }
		runtime "Debug"
		optimize "Off"
		symbols "On"

	filter "configurations:Release"
		defines { "ENGINE_RELEASE" }
		runtime "Release"
		optimize "Speed"
		symbols "On"

	-- NDEBUG for Dist is set at workspace scope (premake5.lua), never here.
	filter "configurations:Dist"
		defines { "ENGINE_DIST" }
		runtime "Release"
		optimize "Full"
		linktimeoptimization "On"
		symbols "On"

	-- premake's xcode4 generator ignores linktimeoptimization: Xcode's own setting turns LTO on for Dist there.
	filter { "configurations:Dist", "action:xcode4" }
		xcodebuildsettings { ["LLVM_LTO"] = "YES" }

	-- Development builds mount engine:// at <repo>/Resources (§2.2). Dist builds read Engine.pak instead.
	-- ENGINE_SHADER_DIRECTORY is the compiled SPIR-V of the configuration being built, ShaderOutputDirectory (above), the
	-- Shaders project's output, which development builds mount as shaders:// (Docs/Decisions/0009-m5-decisions.md).
	-- ENGINE_PYTHON_EXECUTABLE is the interpreter Scripts/Generate.py passed as --python, empty without one: the editor
	-- runs Scripts/BuildNative.py with it to build a game's native module (Docs/Decisions/0030-m19-contract.md decision
	-- 30), by a fixed path, as the Shaders rule runs CompileShaders.py.
	filter "configurations:not Dist"
		defines
		{
			"ENGINE_REPO_ROOT=\"" .. RepositoryRoot .. "\"",
			"ENGINE_SHADER_DIRECTORY=\"" .. ShaderOutputDirectory .. "\"",
			"ENGINE_PYTHON_EXECUTABLE=\"" .. (_OPTIONS["python"] or ""):gsub("\\", "/") .. "\""
		}

	filter {}
end

-- NVRHI + Vulkan-Headers (Vendor/NVRHI/VENDOR.md). The Vulkan loader is loaded at runtime: never link vulkan-1.
function UseNVRHI()
	externalincludedirs { IncludeDir.NVRHI, IncludeDir.VulkanHeaders }
	defines { "VULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1" }

	filter "system:windows"
		defines { "VK_USE_PLATFORM_WIN32_KHR", "NOMINMAX" }

	filter { LinkingKinds }
		links { "NVRHI" }

	filter { LinkingKinds, "system:linux" }
		links { "dl" }

	filter {}
end

-- Jolt Physics (Vendor/JoltPhysics/VENDOR.md). Must stay identical to Vendor/JoltPhysics/premake5.lua; checked at
-- runtime by JPH::VerifyJoltVersionID() and at generation time by Scripts/CheckBuildConfig.py.
function UseJoltPhysics()
	externalincludedirs { IncludeDir.JoltPhysics }
	defines { "JPH_OBJECT_STREAM", "JPH_OBJECT_LAYER_BITS=16", "JPH_CROSS_PLATFORM_DETERMINISTIC" }

	filter "architecture:x86_64"
		defines { "JPH_USE_SSE4_1", "JPH_USE_SSE4_2" }

	filter { "toolset:msc*", "architecture:x86_64" }
		buildoptions { "/arch:SSE4.2" }

	-- Required: Jolt's inline SSE4 intrinsics do not compile without them. GCC, Clang and clang-cl all accept them.
	filter { "toolset:gcc or clang", "architecture:x86_64" }
		buildoptions { "-msse4.2", "-mpopcnt" }

	filter "configurations:Debug"
		defines { "JPH_DEBUG_RENDERER", "JPH_PROFILE_ENABLED", "JPH_ENABLE_ASSERTS" }

	filter { "configurations:Debug", "toolset:msc*" }
		defines { "JPH_FLOATING_POINT_EXCEPTIONS_ENABLED" }

	filter "configurations:Release"
		defines { "JPH_DEBUG_RENDERER", "JPH_PROFILE_ENABLED", "JPH_ENABLE_ASSERTS", "JPH_NO_DEBUG" }

	filter "configurations:Dist"
		defines { "JPH_NO_DEBUG" }

	filter { LinkingKinds }
		links { "JoltPhysics" }

	filter { LinkingKinds, "system:linux" }
		links { "pthread" }

	filter {}
end

-- spdlog, compiled library formatting with std::format (Vendor/spdlog/VENDOR.md).
function UseSpdlog()
	externalincludedirs { IncludeDir.spdlog }
	defines { "SPDLOG_COMPILED_LIB", "SPDLOG_USE_STD_FORMAT" }

	filter "toolset:msc*"
		buildoptions { "/utf-8", "/Zc:__cplusplus" }

	filter { LinkingKinds }
		links { "spdlog" }

	filter { LinkingKinds, "system:linux" }
		links { "pthread" }

	filter {}
end

-- miniaudio (Vendor/miniaudio/VENDOR.md).
function UseMiniaudio()
	externalincludedirs { IncludeDir.miniaudio }
	defines { "MA_NO_ENCODING" }

	filter "system:macosx"
		defines { "MA_NO_RUNTIME_LINKING" }

	filter { LinkingKinds }
		links { "miniaudio" }

	filter { LinkingKinds, "system:linux" }
		links { "dl", "pthread", "m" }

	filter { LinkingKinds, "system:macosx" }
		links { "CoreFoundation.framework", "CoreAudio.framework", "AudioToolbox.framework" }

	filter {}
end

-- Luau (Vendor/Luau/VENDOR.md): include directories only, no defines. `analysis` adds the type checker
-- (LuauAnalysis), which must be linked before Luau.
function UseLuau(analysis)
	externalincludedirs { IncludeDir.Luau }

	if analysis then
		externalincludedirs { IncludeDir.LuauAnalysis }

		filter { LinkingKinds }
			links { "LuauAnalysis" }

		filter {}
	end

	filter { LinkingKinds }
		links { "Luau" }

	filter { LinkingKinds, "system:linux" }
		links { "pthread" }

	filter {}
end

-- GLFW (Vendor/GLFW/VENDOR.md). The engine renders through NVRHI, so GLFW must not include OpenGL headers.
function UseGLFW()
	externalincludedirs { IncludeDir.GLFW }
	defines { "GLFW_INCLUDE_NONE" }

	filter { LinkingKinds }
		links { "GLFW" }

	filter { LinkingKinds, "system:windows" }
		links { "gdi32", "user32", "shell32" }

	filter { LinkingKinds, "system:linux" }
		links { "pthread", "dl", "m", "rt" }

	filter { LinkingKinds, "system:macosx" }
		links { "Cocoa.framework", "IOKit.framework", "CoreFoundation.framework", "QuartzCore.framework" }

	filter {}
end

-- MikkTSpace (Vendor/MikkTSpace/VENDOR.md): tangent generation for the glTF importer (Architecture §7.4). No defines; the
-- library is C with the precise floating-point model, like Jolt (checked by Scripts/CheckBuildConfig.py).
function UseMikkTSpace()
	externalincludedirs { IncludeDir.MikkTSpace }

	filter { LinkingKinds }
		links { "MikkTSpace" }

	filter {}
end

-- Dear ImGui (docking) and ImGuizmo (Vendor/imgui/VENDOR.md, Vendor/ImGuizmo/VENDOR.md). No defines: imconfig.h
-- is unmodified. ImGuizmo depends on ImGui and is linked first.
function UseImGui()
	externalincludedirs { IncludeDir.ImGui, IncludeDir.ImGuizmo }

	filter { LinkingKinds }
		links { "ImGuizmo", "ImGui" }

	filter {}
end

-- The libraries the Runtime's code is compiled against (and, in an executable, linked with): the RuntimeCore library
-- and the Runtime executable (Runtime/premake5.lua), and a game's native module and the Runtime linked with it
-- (Scripts/Premake/NativeModule.lua; Docs/Decisions/0030-m19-contract.md decision 30), so a module's code compiles with
-- the settings of the code it runs beside, instruction set included.
function UseRuntimeDependencies()
	UseImGui()
	UseGLFW()
	UseNVRHI()
	UseJoltPhysics()
	UseSpdlog()
	UseMiniaudio()
	UseMikkTSpace()
	UseLuau(false)
end

-- The Runtime executable (Docs/Architecture.md §2.2, §14.3): RuntimeMain.cpp linked with the RuntimeCore and Engine
-- libraries, plus what decides GetLinkedNativeModule (NoNativeModule.cpp in the prebuilt Runtime, a game's native module
-- in the Runtime Scripts/BuildNative.py links for its export; Docs/Decisions/0030-m19-contract.md decision 30). Called
-- in a ConsoleApp project after ApplyFirstPartySettings(); `location` is the directory of the workspace (the linker map's
-- on Linux and macOS).
function ApplyRuntimeExecutableSettings(location)
	files { RepositoryRoot .. "/Runtime/Source/Runtime/RuntimeMain.cpp" }

	includedirs { RepositoryRoot .. "/Runtime/Source" }

	links
	{
		"RuntimeCore",
		"Engine"
	}

	UseRuntimeDependencies()

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
		linkoptions { "-Wl,-Map=" .. location .. "/bin/" .. OutputDir .. "/%{prj.name}/%{prj.name}.map" }

	filter "system:macosx"
		linkoptions { "-Wl,-map," .. location .. "/bin/" .. OutputDir .. "/%{prj.name}/%{prj.name}.map" }

	-- Exported macOS games bundle the Vulkan loader and ICD in Contents/Frameworks (§16).
	filter "system:macosx"
		linkoptions { "-Wl,-rpath,@executable_path/../Frameworks" }

	-- Exported Linux games look for shared libraries next to the executable first (§14.1, §16: RPATH $ORIGIN). The single
	-- quotes keep the shell from expanding it; make and ninja both turn $$ into $ (the portability stage checks the
	-- generated link lines, Docs/Decisions/0024-m15-contract.md decision 18).
	filter "system:linux"
		linkoptions { "-Wl,-rpath,'$$ORIGIN'" }

	filter {}
end
