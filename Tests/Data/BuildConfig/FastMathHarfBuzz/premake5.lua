-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0033-m22-contract.md decision 41): the real workspace with one
-- defect, on which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathHarfBuzz
--
-- Defect: HarfBuzz is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise
-- model. Its shaped glyph positions reach the UI's layout and the cooked fonts, which must be identical in every
-- configuration (Architecture §2.2; Roadmap M22: shaped text is byte-identical across configurations).

include "../../../.."

workspace (WorkspaceName)

project "HarfBuzz"
	floatingpoint "Fast"
