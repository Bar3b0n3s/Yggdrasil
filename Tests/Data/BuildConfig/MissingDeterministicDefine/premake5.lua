-- Fixture for Scripts/CheckBuildConfig.py (Roadmap M0): the real workspace with one defect, on which the check must
-- fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/MissingDeterministicDefine
--
-- Defect: the Engine project includes Jolt headers without JPH_CROSS_PLATFORM_DETERMINISTIC, which JoltPhysics and
-- every consumer must define in every configuration (Architecture §2.2, §9.1).

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	removedefines { "JPH_CROSS_PLATFORM_DETERMINISTIC" }
