#include "EnginePCH.h"
#include "Engine/Renderer/EditorOverlay.h"

namespace Engine {

	Status AppendEditorOverlay(const RenderSnapshot&, DebugDrawList&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Status AppendWireframeOverlay(const RenderSnapshot&, AssetManager&, DebugDrawList&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 portable wireframe is not implemented"));
	}

}
