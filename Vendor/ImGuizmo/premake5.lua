-- ImGuizmo (master @ 18cef5e031d8c6973d80284c67f60549fafd78c1)
-- Upstream: https://github.com/CedricGuillemet/ImGuizmo  (see VENDOR.md)
--
-- Builds only the 3D transform gizmo (src/ImGuizmo.cpp). The other upstream
-- widgets (ImSequencer, GraphEditor, ImCurveEdit, ImGradient, ImVectorEditor)
-- are not vendored.
--
-- Consumers: includedirs { "%{wks.location}/Vendor/imgui", "%{wks.location}/Vendor/ImGuizmo/src" },
-- links { "ImGuizmo", "ImGui" }. Include "imgui.h" before "ImGuizmo.h".

project "ImGuizmo"
	kind "StaticLib"
	language "C++"
	cppdialect "C++17"
	staticruntime "off"

	targetdir ("%{wks.location}/bin/" .. OutputDir .. "/%{prj.name}")
	objdir ("%{wks.location}/bin-int/" .. OutputDir .. "/%{prj.name}")

	files
	{
		"src/ImGuizmo.h",
		"src/ImGuizmo.cpp"
	}

	includedirs
	{
		"src",
		"../imgui"
	}

	filter "system:windows"
		systemversion "latest"

	filter "system:linux"
		pic "On"

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
