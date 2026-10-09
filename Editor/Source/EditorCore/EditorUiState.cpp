#include "EditorPCH.h"
#include "EditorCore/EditorUiState.h"

namespace Engine {

	std::string_view EditorPanelToString(EditorPanel /*panel*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status EditorUiState::SetPanelOpen(EditorPanel /*panel*/, bool /*open*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void EditorUiState::SetSelectedAsset(AssetHandle /*asset*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorUiState::CompleteFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	void EditorUiState::ResetLayout(bool /*hasProject*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
