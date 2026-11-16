-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0045-m30-navigation.md): the real workspace with one defect, on
-- which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathDetour
--
-- Defect: Detour is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise model.
-- Detour's path, closest-point and surface-ray queries run on the simulation path, so a path must be
-- identical in Debug, Release and Dist (Architecture §2.2, §9.1).

include "../../../.."

workspace (WorkspaceName)

project "Detour"
	floatingpoint "Fast"
