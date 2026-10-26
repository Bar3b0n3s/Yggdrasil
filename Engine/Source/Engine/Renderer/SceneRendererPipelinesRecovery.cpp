#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"

#include <utility>

// The pipeline set's Release hook in the recreation protocol (SceneRendererPipelines::ReleaseGpuObjects;
// Docs/Decisions/0029-m18-contract.md decisions 26 and 27): every pass is released when the device is lost, and
// CreatePipelines, the set's Recreate hook (SceneRendererPipelines::Create registers both), creates them again on the
// recovered device.
//
// Release replaces the whole state with a new one that receives only what survives a loss (the back-references, the
// registration and the generation), so every pass, pipeline, binding layout, sampler and startup resource goes with the
// released state, the lazily created debug-view pipelines, the QuadBatcher and the output stage's UpscalePass included, and
// a pass a later milestone adds to the state is released without code of its own (the placement rule's "resets every
// member but"). The released passes only drop their handles: nothing is called on the device but their release, so it also
// runs on a lost device in the middle of a frame the loss ended.

namespace Engine {

	void SceneRendererPipelines::ReleaseGpuObjects()
	{
		State& state = *m_State;
		State kept;
		kept.Device = state.Device;
		kept.Factory = state.Factory;
		kept.Generation = state.Generation;
		// Moving a registration over an empty one calls nothing on the registry, so this runs inside ReleaseAll.
		kept.Recreation = std::move(state.Recreation);
		state = std::move(kept);
	}

}
