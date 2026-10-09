#include "EditorPCH.h"
#include "Editor/Viewport/GizmoOverlay.h"

namespace Engine {

	Status DrawGizmoOverlay(EditorContext& /*context*/, GizmoController& /*controller*/, const CameraData& /*camera*/, const EditorViewportRect& /*rectangle*/, GizmoSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
