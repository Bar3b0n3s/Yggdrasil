-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathComponents
--
-- Defect: JoltPhysics is built with components of -ffast-math (finite math only, no signed zeros, no
-- trapping math) on GCC and Clang, which change floating-point results although -ffast-math itself is absent.

include "../../../.."

workspace (WorkspaceName)

project "JoltPhysics"
	filter { "toolset:gcc or clang", "action:not vs*" }
		buildoptions { "-ffinite-math-only", "-fno-signed-zeros", "-fno-trapping-math" }

	filter {}
