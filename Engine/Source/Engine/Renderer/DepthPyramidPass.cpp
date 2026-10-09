#include "EnginePCH.h"
#include "Engine/Renderer/DepthPyramidPass.h"

namespace Engine {

	struct DepthPyramidPass::State
	{
	};

	DepthPyramidPass::DepthPyramidPass(ConstructionKey)
	{
		ENGINE_CONTRACT_STUB();
	}

	DepthPyramidPass::~DepthPyramidPass() = default;

	Result<Scope<DepthPyramidPass>> DepthPyramidPass::Create(GraphicsDevice&, PipelineFactory&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	std::vector<PipelineLayoutDescription> DepthPyramidPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	float ReconstructLinearViewDepth(const CameraData&, float)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	glm::vec3 ReconstructViewPosition(const CameraData&, const glm::vec2&, float)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	nvrhi::TextureDesc DepthPyramidPass::GetTargetDesc(uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Status DepthPyramidPass::Record(nvrhi::ICommandList&, RenderRecordingContext&, PassBindingCache&, const DepthPyramidInputs&)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 contract is not implemented"));
	}

	Result<DepthReductionFootprint> ComputeDepthReductionFootprint(uint32_t, uint32_t, uint32_t, uint32_t)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M9 conservative depth reduction is not implemented"));
	}

}
