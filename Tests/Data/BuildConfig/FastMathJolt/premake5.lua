-- Fixture for Scripts/CheckBuildConfig.py (Roadmap M0): the real workspace with one defect, on which the check must
-- fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathJolt
--
-- Defect: JoltPhysics is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math), upstream's
-- non-deterministic setting, instead of the precise model required for cross-configuration determinism
-- (Architecture §2.2, §9.1).

include "../../../.."

workspace (WorkspaceName)

project "JoltPhysics"
	floatingpoint "Fast"
