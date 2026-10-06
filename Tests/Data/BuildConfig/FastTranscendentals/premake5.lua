-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastTranscendentals
--
-- Defect: the Engine project is built with /Qfast_transcendentals on MSVC, which replaces the CRT
-- transcendental functions with faster, less precise inline code.

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	filter "toolset:msc*"
		buildoptions { "/Qfast_transcendentals" }

	filter {}
