#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"

// The scene renderer's last three stages (SceneRenderer.h; Docs/Decisions/0029-m18-contract.md decisions 1 and 6): the
// scene-resolution overlays (game Text, then M22's reserved Ui slot), the output stage's integer upscale with its
// letterbox, and the view-resolution editor overlays.

namespace Engine {

	void SceneRenderer::State::RecordSceneOverlayStage(Detail::SceneRendererFrame& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void SceneRenderer::State::RecordOutputStage(Detail::SceneRendererFrame& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void SceneRenderer::State::RecordEditorOverlayStage(Detail::SceneRendererFrame& /*frame*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
