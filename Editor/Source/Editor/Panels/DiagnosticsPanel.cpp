#include "EditorPCH.h"
#include "Editor/Panels/DiagnosticsPanel.h"

namespace Engine {

	Status DiagnosticsPanel::Draw(EditorPanelContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
