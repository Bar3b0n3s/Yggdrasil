#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorCamera.h"

namespace Engine {

	Result<ExplicitRenderCamera> EditorCamera::Navigate(const ExplicitRenderCamera& /*camera*/, const EditorCameraInput& /*input*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Result<ExplicitRenderCamera> EditorCamera::FrameBounds(const ExplicitRenderCamera& /*camera*/, const glm::vec3& /*minimum*/, const glm::vec3& /*maximum*/, float /*aspect*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
