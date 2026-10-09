#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

namespace Engine {

	Status EditorContext::SetSelection(std::vector<UUID> /*selection*/, SceneTarget /*target*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
