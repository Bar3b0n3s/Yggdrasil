#include "Engine/Native/NativeApi.h"

// The prebuilt Runtime's GetLinkedNativeModule (Engine/Native/NativeApi.h; Docs/Decisions/0030-m19-contract.md decision 30):
// it links no module. The Runtime of an exported game with Native/ is linked by Scripts/BuildNative.py --kind runtime
// without this file, and the module's ENGINE_NATIVE_MODULE defines the function there instead; stream N-A moves this file
// out of what that link takes (the Runtime project's split).

namespace Engine {

	NativeModuleEntryPoint GetLinkedNativeModule()
	{
		return NativeModuleEntryPoint{ .ApiVersion = NativeApiVersion, .Register = nullptr };
	}

}
