#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"

// The pipeline set's Release hook in the recreation protocol (SceneRendererPipelines::ReleaseGpuObjects;
// Docs/Decisions/0029-m18-contract.md decisions 26 and 27): every pass is released when the device is lost, and
// CreatePipelines creates them again on the recovered device.

namespace Engine {

	void SceneRendererPipelines::ReleaseGpuObjects()
	{
		ENGINE_CONTRACT_STUB();
	}

}
