#include "EnginePCH.h"
#include "Engine/Renderer/QuadBatcher.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/RenderStats.h"

namespace Engine {

	struct QuadBatcher::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
	};

	NineSliceExpansion ExpandNineSlice(const NineSliceSource& /*source*/, glm::vec2 /*size*/, float /*borderScale*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	glm::mat4 MakeQuadPixelTransform(uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::mat4(1.0f);
	}

	QuadBatcher::QuadBatcher(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	QuadBatcher::~QuadBatcher() = default;

	Result<Scope<QuadBatcher>> QuadBatcher::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 QuadBatcher contract (Docs/Decisions/0029-m18-contract.md decision 10, stream F)"));
	}

	std::vector<PipelineLayoutDescription> QuadBatcher::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	nvrhi::FramebufferInfo QuadBatcher::GetFramebufferInfo(QuadBatchTarget /*target*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	uint32_t QuadBatcher::GetPipelineCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	nvrhi::ISampler* QuadBatcher::GetSampler(QuadSampler /*sampler*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<uint32_t> QuadBatcher::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const QuadBatchInputs& /*inputs*/,
		RenderPassCounters& /*counters*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported, "M18 QuadBatcher contract (Docs/Decisions/0029-m18-contract.md decision 10, stream F)"));
	}

}
