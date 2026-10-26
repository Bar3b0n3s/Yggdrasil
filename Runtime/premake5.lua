-- Runtime: the executable that runs exported games (Docs/Architecture.md §2.2, §14.3). It links Engine without the
-- editor libraries and uses no precompiled header.
--
-- The code is split (Docs/Decisions/0030-m19-contract.md decision 30): the RuntimeCore library holds the application,
-- and the Runtime executable adds RuntimeMain.cpp and NoNativeModule.cpp, which answers GetLinkedNativeModule with no
-- module. The Runtime of a game with a native module is the same executable with the module's sources instead of
-- NoNativeModule.cpp, linked by Scripts/BuildNative.py --kind runtime (Scripts/Premake/NativeModule.lua) from this
-- configuration's RuntimeCore and Engine libraries; ApplyRuntimeExecutableSettings() (Dependencies.lua) holds what both
-- executables share.

project "RuntimeCore"
	kind "StaticLib"
	ApplyFirstPartySettings()

	files
	{
		"Source/Runtime/**.h",
		"Source/Runtime/**.cpp"
	}

	removefiles
	{
		"Source/Runtime/RuntimeMain.cpp",
		"Source/Runtime/NoNativeModule.cpp"
	}

	includedirs
	{
		"Source"
	}

	UseRuntimeDependencies()

project "Runtime"
	kind "ConsoleApp"
	ApplyFirstPartySettings()

	files
	{
		"Source/Runtime/NoNativeModule.cpp"
	}

	ApplyRuntimeExecutableSettings(WorkspaceLocation)
