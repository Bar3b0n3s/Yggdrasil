#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"

namespace Engine {

	struct SceneRendererPipelines::State
	{
		uint32_t PipelineCount = 0;
	};

	struct SceneRenderer::State
	{
		SceneRenderStats Stats{};
	};

	SceneRendererPipelines::SceneRendererPipelines(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SceneRendererPipelines::~SceneRendererPipelines() = default;

	Result<Scope<SceneRendererPipelines>> SceneRendererPipelines::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the scene renderer is not implemented yet (M7 stream B)");
	}

	uint32_t SceneRendererPipelines::GetPipelineCount() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->PipelineCount;
	}

	std::vector<PipelineLayoutDescription> SceneRendererPipelines::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	SceneRenderer::SceneRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SceneRenderer::~SceneRenderer() = default;

	Result<Scope<SceneRenderer>> SceneRenderer::Create(GraphicsDevice& /*device*/, const SceneRendererPipelines& /*pipelines*/,
		GpuResourceCache& /*cache*/, AssetManager& /*assets*/, const SceneRendererSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the scene renderer is not implemented yet (M7 stream B)");
	}

	Status SceneRenderer::Resize(uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the scene renderer is not implemented yet (M7 stream B)");
	}

	Status SceneRenderer::Render(nvrhi::ICommandList& /*commandList*/, const RenderSnapshot& /*snapshot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the scene renderer is not implemented yet (M7 stream B)");
	}

	nvrhi::ITexture* SceneRenderer::GetFinalTexture() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	uint32_t SceneRenderer::GetWidth() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t SceneRenderer::GetHeight() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const SceneRenderStats& SceneRenderer::GetLastStats() const
	{
		return m_State->Stats;
	}

}
