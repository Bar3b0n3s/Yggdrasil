-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/ClangOnlyContraction
--
-- Defect: JoltPhysics contracts floating-point expressions into FMA with Clang on Linux only (the CI
-- Clang job). Only a check of the clang toolset's generated projects sees it.

include "../../../.."

workspace (WorkspaceName)

project "JoltPhysics"
	filter { "toolset:clang", "system:linux" }
		buildoptions { "-ffp-contract=fast" }

	filter {}
