-- Fixture for Scripts/CheckBuildConfig.py (Roadmap M0): the real workspace with one defect, on which the check must
-- fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/MismatchedJoltDefine
--
-- Defect: the Tests project includes Jolt headers without JPH_PROFILE_ENABLED, which JoltPhysics defines in Debug and
-- Release. Jolt's class layouts and JPH_VERSION_ID depend on it, so this is an ABI mismatch.

include "../../../.."

workspace (WorkspaceName)

project "Tests"
	removedefines { "JPH_PROFILE_ENABLED" }
