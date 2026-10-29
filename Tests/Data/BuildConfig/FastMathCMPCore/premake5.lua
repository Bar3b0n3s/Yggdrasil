-- Fixture for Scripts/CheckBuildConfig.py (Docs/Decisions/0034-m23-contract.md): the real workspace with one defect, on
-- which the check must fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/FastMathCMPCore
--
-- Defect: CMP_Core is built with floatingpoint "Fast" (MSVC /fp:fast, GCC/Clang -ffast-math) instead of the precise
-- model. Its compressed texture blocks are cooked into the asset cache, which must be identical from every
-- configuration of one compiler (Architecture §2.2, §7.5).

include "../../../.."

workspace (WorkspaceName)

project "CMP_Core"
	floatingpoint "Fast"
