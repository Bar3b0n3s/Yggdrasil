-- Fixture for Scripts/CheckBuildConfig.py: the real workspace with one defect, on which the check must fail.
-- Run: python Scripts/CheckBuildConfig.py --workspace Tests/Data/BuildConfig/LuauVectorSize
--
-- Defect: the Engine project defines LUA_VECTOR_SIZE=4, which changes the layout of Luau's TValue
-- against the Luau library built with the default of 3 (Vendor/Luau/VENDOR.md).

include "../../../.."

workspace (WorkspaceName)

project "Engine"
	defines { "LUA_VECTOR_SIZE=4" }
