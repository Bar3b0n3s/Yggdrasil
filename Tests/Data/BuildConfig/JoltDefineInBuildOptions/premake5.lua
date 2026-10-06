-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/JoltDefineInBuildOptions
--
-- Defect: the Engine project defines JPH_DOUBLE_PRECISION through its compiler options instead of
-- its define list. JoltPhysics is built in single precision, so every Jolt type in Engine has the wrong layout.

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	filter "toolset:msc*"
		buildoptions { "/DJPH_DOUBLE_PRECISION" }

	filter "toolset:not msc*"
		buildoptions { "-DJPH_DOUBLE_PRECISION" }

	filter {}
