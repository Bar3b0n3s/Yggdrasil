#include "EnginePCH.h"
#include "Engine/Graphics/PipelineFactory.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream C (shader pipeline) implements the reflection check and pipeline creation of
// Architecture §8.4 and §8.12.

namespace Engine {

	Status ValidatePipelineLayout(const PipelineLayoutDescription& /*description*/, ShaderLibrary& /*shaders*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ValidatePipelineLayout is not implemented yet");
	}

	PipelineFactory::PipelineFactory(GraphicsDevice& /*device*/, ShaderLibrary& shaders)
		: m_Shaders(&shaders)
	{
		ENGINE_CONTRACT_STUB();
	}

	PipelineFactory::~PipelineFactory() = default;

	Result<GraphicsPipeline> PipelineFactory::CreateGraphicsPipeline(const GraphicsPipelineSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PipelineFactory::CreateGraphicsPipeline is not implemented yet");
	}

	Result<ComputePipeline> PipelineFactory::CreateComputePipeline(const ComputePipelineSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "PipelineFactory::CreateComputePipeline is not implemented yet");
	}

}
