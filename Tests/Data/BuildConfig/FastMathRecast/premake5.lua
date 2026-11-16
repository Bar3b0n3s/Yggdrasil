-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0045-m30-navigation.md): the real workspace with one defect, on
-- which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathRecast
--
-- Defect: Recast is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise model.
-- A bake's NavMesh file is written from Recast's polygon and detail meshes, so two bakes of one scene from the
-- Debug and Release editors must agree byte for byte (Architecture §2.2, §9.1).

include "../../../.."

workspace (WorkspaceName)

project "Recast"
	floatingpoint "Fast"
