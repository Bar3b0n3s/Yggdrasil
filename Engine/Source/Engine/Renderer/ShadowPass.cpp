#include "EnginePCH.h"
#include "Engine/Renderer/ShadowPass.h"

namespace Engine {

	struct ShadowPass::State
	{
	};

	ShadowPass::ShadowPass(ConstructionKey)
	{
		ENGINE_CONTRACT_STUB();
	}

	ShadowPass::~ShadowPass() = default;

	Result<Scope<ShadowPass>> ShadowPass::Create(GraphicsDevice&, PipelineFactory&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	std::vector<PipelineLayoutDescription> ShadowPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::TextureDesc ShadowPass::GetCascadeTargetDesc(uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::TextureDesc ShadowPass::GetAtlasTargetDesc()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<ShadowRenderResult> ShadowPass::Record(nvrhi::ICommandList&, RenderRecordingContext&, PassBindingCache&, GpuResourceCache&, AssetManager&, const ShadowRenderInputs&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

}
