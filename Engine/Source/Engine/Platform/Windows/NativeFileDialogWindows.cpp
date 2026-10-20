#include "EnginePCH.h"
#include "Engine/Platform/NativeFileDialog.h"

// The native file dialog on Windows: nativefiledialog-extended's IFileDialog backend on a thread of its own, in a
// single-threaded COM apartment (NativeFileDialog.h; Docs/Decisions/0029-m18-contract.md decisions 19 and 21).

#if defined(ENGINE_PLATFORM_WINDOWS)

namespace Engine {

	struct NativeFileDialog::State
	{
	};

	NativeFileDialog::NativeFileDialog(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	NativeFileDialog::~NativeFileDialog() = default;

	Result<Scope<NativeFileDialog>> NativeFileDialog::Open(const FileDialogRequest& /*request*/, const Window* /*owner*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 FileDialog contract (Docs/Decisions/0029-m18-contract.md decision 19, stream G)"));
	}

	Result<FileDialogOutcome> NativeFileDialog::Poll()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 FileDialog contract (Docs/Decisions/0029-m18-contract.md decision 19, stream G)"));
	}

	void NativeFileDialog::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

}

#endif
