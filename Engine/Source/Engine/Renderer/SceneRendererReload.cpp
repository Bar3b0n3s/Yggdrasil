#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"

// The editor's shader hot reload (SceneRendererPipelines::ReloadShaders; Docs/Decisions/0029-m18-contract.md decision 3):
// the factory's ShaderLibrary re-reads the compiled output around CreatePipelines, which swaps the pipelines in only when
// every one of them could be created.

namespace Engine {

	Status SceneRendererPipelines::ReloadShaders()
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 shader hot reload contract (Docs/Decisions/0029-m18-contract.md decision 3, stream B)"));
	}

}
