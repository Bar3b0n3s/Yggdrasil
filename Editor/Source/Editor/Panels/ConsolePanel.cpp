#include "EditorPCH.h"
#include "Editor/Panels/ConsolePanel.h"

namespace Engine {

	Status ConsolePanel::Draw(EditorPanelContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
