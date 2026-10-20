#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Engine/Renderer/Private/SceneRendererState.h"

// The editor's shader hot reload (SceneRendererPipelines::ReloadShaders; Docs/Decisions/0029-m18-contract.md decision 3):
// the factory's ShaderLibrary re-reads the compiled output around CreatePipelines, which swaps the pipelines in only when
// every one of them could be created.

namespace Engine {

	Status SceneRendererPipelines::ReloadShaders()
	{
		ShaderLibrary& shaders = m_State->Factory->GetShaderLibrary();
		shaders.BeginReload();
		Status created = CreatePipelines();
		// A failed creation changed no pipeline, so the library goes back to the variants they were created from.
		shaders.EndReload(created.has_value());
		if (!created)
			return std::unexpected(std::move(created).error().WithContext("while reloading the shaders; the previous pipelines stay in use"));
		ENGINE_CORE_INFO("Reloaded the shaders: the scene renderer's {} pipelines are generation {}", GetPipelineCount(), GetGeneration());
		return {};
	}

}
