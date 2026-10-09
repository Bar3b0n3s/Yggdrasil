#include "EnginePCH.h"
#include "Engine/Renderer/GtaoPass.h"

namespace Engine {

	struct GtaoPass::State
	{
	};

	GtaoPass::GtaoPass(ConstructionKey)
	{
		ENGINE_CONTRACT_STUB();
	}

	GtaoPass::~GtaoPass() = default;

	Result<Scope<GtaoPass>> GtaoPass::Create(GraphicsDevice&, PipelineFactory&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	std::vector<PipelineLayoutDescription> GtaoPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<float> ComputeGtaoScreenRadius(const CameraData&, float, float)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	uint32_t GetGtaoSliceCount(RenderSsaoQuality)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	nvrhi::TextureDesc GtaoPass::GetTargetDesc(uint32_t, uint32_t, bool)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<nvrhi::ITexture*> GtaoPass::Record(nvrhi::ICommandList&, RenderRecordingContext&, PassBindingCache&, const GtaoInputs&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

}
