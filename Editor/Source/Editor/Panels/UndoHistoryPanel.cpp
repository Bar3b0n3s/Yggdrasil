#include "EditorPCH.h"
#include "Editor/Panels/UndoHistoryPanel.h"

namespace Engine {

	Status UndoHistoryPanel::Draw(EditorPanelContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
