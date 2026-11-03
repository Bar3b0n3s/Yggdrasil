-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0034-m23-contract.md decisions 14 and 21): the real workspace
-- with one defect, on which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace
-- Tests/Data/BuildConfig/FastMathTinyExr
--
-- Defect: tinyexr is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise
-- model. An OpenEXR image's decoded texels (its half-float conversion among them) are baked into a cooked environment,
-- whose cache key does not include the build configuration (Architecture §2.2, §7.5).

include "../../../.."

workspace (WorkspaceName)

project "tinyexr"
	floatingpoint "Fast"
