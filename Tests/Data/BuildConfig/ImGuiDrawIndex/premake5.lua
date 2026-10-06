-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/ImGuiDrawIndex
--
-- Defect: the Engine project overrides ImDrawIdx, which changes ImDrawList against the ImGui library
-- (Vendor/imgui/VENDOR.md: configuration defines must be identical in ImGui, ImGuizmo and every consumer).

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	defines { "ImDrawIdx=unsigned int" }
