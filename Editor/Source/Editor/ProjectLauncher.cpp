#include "EditorPCH.h"
#include "Editor/ProjectLauncher.h"

namespace Engine {

	Status ProjectLauncher::Draw(EditorPanelContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
