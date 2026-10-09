#include "EditorPCH.h"
#include "EditorCore/Viewport/EditorViewportState.h"

namespace Engine {

	Result<glm::uvec2> EditorViewportState::GetPixelSize(ViewportView) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	Status EditorViewportState::SetPixelSize(ViewportView, const glm::uvec2&)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void EditorViewportState::SetSceneSize(const glm::uvec2& /*size*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status EditorViewportState::SetGameResolution(const glm::uvec2& /*size*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	std::optional<EditorViewportPixel> ToViewportPixel(const EditorViewportRect& /*rectangle*/, const glm::vec2& /*point*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	Status EditorViewportState::SetCamera(const ExplicitRenderCamera& /*camera*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

	void EditorViewportState::SetOptions(const EditorViewportOptions& /*options*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status EditorViewportState::SetDebugView(RenderDebugView /*view*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "M10 contract stub");
	}

}
