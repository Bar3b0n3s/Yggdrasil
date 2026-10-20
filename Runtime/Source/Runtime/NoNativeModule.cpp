#include "Engine/Native/NativeApi.h"

// The prebuilt Runtime's GetLinkedNativeModule (Engine/Native/NativeApi.h; Docs/Decisions/0030-m19-contract.md decision 30):
// it links no module. Only the Runtime executable compiles this file (Runtime/premake5.lua); the Runtime of an exported
// game with Native/ is the same executable linked by Scripts/BuildNative.py --kind runtime with the module's sources
// instead, whose ENGINE_NATIVE_MODULE defines the function there.

namespace Engine {

	NativeModuleEntryPoint GetLinkedNativeModule()
	{
		return NativeModuleEntryPoint{ .ApiVersion = NativeApiVersion, .Register = nullptr };
	}

}
