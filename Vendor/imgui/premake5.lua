-- Dear ImGui (docking branch) v1.92.9b-docking
-- Upstream: https://github.com/ocornut/imgui  (see VENDOR.md)
--
-- Builds the platform-independent ImGui core plus misc/cpp/imgui_stdlib.
-- Backends are NOT compiled here: backends/imgui_impl_glfw.cpp is vendored
-- for the engine to compile in its own translation unit (it needs GLFW's
-- include path and native-access defines), and rendering goes through the
-- engine's own NVRHI renderer.
--
-- Consumers: includedirs { "%{wks.location}/Vendor/imgui" }, links { "ImGui" }.
-- Any imconfig.h-style define (e.g. IMGUI_DISABLE_OBSOLETE_FUNCTIONS) must be
-- added to this project AND to every consumer (ImGuizmo, Engine, Editor, ...).

project "ImGui"
	kind "StaticLib"
	language "C++"
	cppdialect "C++11"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"imconfig.h",
		"imgui.h",
		"imgui_internal.h",
		"imstb_rectpack.h",
		"imstb_textedit.h",
		"imstb_truetype.h",
		"imgui.cpp",
		"imgui_demo.cpp",
		"imgui_draw.cpp",
		"imgui_tables.cpp",
		"imgui_widgets.cpp",
		"misc/cpp/imgui_stdlib.h",
		"misc/cpp/imgui_stdlib.cpp"
	}

	includedirs
	{
		"."
	}

	filter "system:windows"
		systemversion "latest"
		-- user32/kernel32 (clipboard), shell32 (OpenInShell) and imm32 (IME) are
		-- requested by imgui.cpp through '#pragma comment(lib, ...)' under MSVC.

	filter "system:linux"
		pic "On"

	filter "system:macosx"
		-- No extra sources or frameworks: the optional Carbon clipboard path
		-- (IMGUI_ENABLE_OSX_DEFAULT_CLIPBOARD_FUNCTIONS) is not enabled.

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
