-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 5): the
-- real workspace with one defect, on which the check must fail. Run:
-- python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathMeshOptimizer
--
-- Defect: meshoptimizer is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the
-- precise model. Generated mesh levels are cooked into meshes, which must be identical from a Debug and a Release editor
-- (Architecture §2.2, §7.5).

include "../../../.."

workspace (WorkspaceName)

project "meshoptimizer"
	floatingpoint "Fast"
