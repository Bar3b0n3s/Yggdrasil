#include "EditorPCH.h"
#include "Editor/FolderPicker.h"

namespace Engine {

	Status FolderPicker::Open(std::string_view /*title*/, const std::filesystem::path& /*initialDirectory*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<std::optional<std::filesystem::path>> FolderPicker::Draw()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void FolderPicker::Cancel()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool FolderPicker::IsOpen() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
