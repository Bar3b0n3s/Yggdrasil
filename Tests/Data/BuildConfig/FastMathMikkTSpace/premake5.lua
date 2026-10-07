-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0010-m6-decisions.md): the real workspace with one defect, on
-- which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathMikkTSpace
--
-- Defect: MikkTSpace is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise
-- model. Its generated tangents are cooked into meshes, which must be identical in every configuration (Architecture
-- §2.2, §7.4).

include "../../../.."

workspace (WorkspaceName)

project "MikkTSpace"
	floatingpoint "Fast"
