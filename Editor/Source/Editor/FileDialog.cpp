#include "EditorPCH.h"
#include "Editor/FileDialog.h"

namespace Engine {

	FileDialogBackend MakeNativeFileDialogBackend(const Window& /*owner*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status FileDialog::Open(const FileDialogRequest& /*request*/, const FileDialogBackend& /*backend*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 FileDialog contract (Docs/Decisions/0029-m18-contract.md decision 19, stream G)"));
	}

	Result<std::optional<std::filesystem::path>> FileDialog::Draw()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 FileDialog contract (Docs/Decisions/0029-m18-contract.md decision 19, stream G)"));
	}

	void FileDialog::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool FileDialog::IsOpen() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool FileDialog::IsNative() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
