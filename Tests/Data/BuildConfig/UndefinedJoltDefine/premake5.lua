-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/UndefinedJoltDefine
--
-- Defect: the Tests project undefines JPH_PROFILE_ENABLED, which JoltPhysics defines in Debug and
-- Release. The define list still contains it, so only a check that applies undefines in command-line order (MSVC
-- UndefinePreprocessorDefinitions, GCC/Clang -U) sees the ABI mismatch.

include "../../../.."

workspace (WorkspaceName)

project "Tests"
	undefines { "JPH_PROFILE_ENABLED" }
