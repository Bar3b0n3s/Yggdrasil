#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"

// A scene renderer's hooks in the recreation protocol (SceneRenderer::ReleaseGpuObjects and RecreateGpuObjects;
// Docs/Decisions/0029-m18-contract.md decisions 26 and 27): its per-view objects are released when the device is lost and
// created again on the recovered device.

namespace Engine {

	void SceneRenderer::ReleaseGpuObjects()
	{
		ENGINE_CONTRACT_STUB();
	}

	Status SceneRenderer::RecreateGpuObjects()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 recreation protocol contract (Docs/Decisions/0029-m18-contract.md decision 27, stream B)"));
	}

}
