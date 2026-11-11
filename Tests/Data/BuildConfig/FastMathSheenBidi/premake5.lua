-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0033-m22-contract.md decision 41): the real workspace with one
-- defect, on which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathSheenBidi
--
-- Defect: SheenBidi is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise
-- model. Its bidirectional runs reach the UI's layout, which must be identical in every configuration (Architecture
-- §2.2; Roadmap M22: shaped text is byte-identical across configurations).

include "../../../.."

workspace (WorkspaceName)

project "SheenBidi"
	floatingpoint "Fast"
