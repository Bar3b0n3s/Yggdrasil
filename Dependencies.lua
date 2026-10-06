-- Dependencies.lua: include directories, first-party project settings and one Use<Lib>() function per vendored
-- library (Docs/Architecture.md §2.2).
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
	filter "configurations:not Dist"
		defines { "ENGINE_REPO_ROOT=\"" .. RepositoryRoot .. "\"" }

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

-- Dear ImGui (docking) and ImGuizmo (Vendor/imgui/VENDOR.md, Vendor/ImGuizmo/VENDOR.md). No defines: imconfig.h
-- is unmodified. ImGuizmo depends on ImGui and is linked first.
function UseImGui()
	externalincludedirs { IncludeDir.ImGui, IncludeDir.ImGuizmo }

	filter { LinkingKinds }
		links { "ImGuizmo", "ImGui" }

	filter {}
end
