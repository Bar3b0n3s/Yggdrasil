-- Fixture for Scripts/CheckBuildConfig.py (M13 review F01): the real workspace with one defect, on which the check must
-- fail. Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/LuauContraction
--
-- Defect: the Luau VM is built without its precise floating-point options, so GCC (default -ffp-contract=fast), Clang
-- and clang-cl (default "on") may contract script arithmetic into FMA and premake's xcode4 Dist build keeps -ffast-math.
-- Scripts' gameplay arithmetic runs in the Luau interpreter and its vector and math libraries (Architecture §2.2,
-- §11.1), so Debug, Release and Dist must evaluate it alike.

include "../../../.."

workspace (WorkspaceName)

project "Luau"
	removebuildoptions { "-fno-fast-math", "-ffp-contract=off", "/clang:-fno-fast-math", "/clang:-ffp-contract=off" }
