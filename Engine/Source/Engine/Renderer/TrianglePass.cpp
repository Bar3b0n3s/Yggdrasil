#include "EnginePCH.h"
#include "Engine/Renderer/TrianglePass.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream C (shader pipeline) implements the pipeline, the fixed camera and the draw.
// Until then Create fails with Unsupported.

namespace Engine {

	TrianglePass::TrianglePass(ConstructionKey /*key*/)
	{
	}

	TrianglePass::~TrianglePass() = default;

	Result<Scope<TrianglePass>> TrianglePass::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/,
		const TrianglePassSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "TrianglePass::Create is not implemented yet");
	}

	void TrianglePass::Render(nvrhi::ICommandList& /*commandList*/, nvrhi::IFramebuffer& /*framebuffer*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	PipelineLayoutDescription TrianglePass::GetLayoutDescription()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	ViewConstants TrianglePass::MakeViewConstants(uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
