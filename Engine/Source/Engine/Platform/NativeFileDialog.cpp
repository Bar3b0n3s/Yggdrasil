#include "EnginePCH.h"
#include "Engine/Platform/NativeFileDialog.h"

// The platform-independent part of the native file dialogs: the request rules both of the editor's dialogs share
// (Docs/Decisions/0029-m18-contract.md decision 19). The dialogs themselves live in Platform/Windows/ and Platform/Posix/.

namespace Engine {

	Status ValidateFileDialogRequest(const FileDialogRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 FileDialog contract (Docs/Decisions/0029-m18-contract.md decision 19, stream G)"));
	}

}
