-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/PartialAddressSanitizer
--
-- Defect: in the hardening stage's AddressSanitizer workspace (premake --sanitize=address), the Engine project is
-- built without AddressSanitizer while every other project has it. MSVC's STL marks the translation units that use its
-- container annotations, so linking instrumented and uninstrumented C++ together fails or misreports container
-- overflows (Docs/Decisions/0024-m15-contract.md decision 12). Only the windows-asan target sees it.

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	removesanitize { "Address" }
