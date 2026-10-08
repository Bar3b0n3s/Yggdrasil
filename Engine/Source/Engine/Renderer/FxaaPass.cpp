#include "EnginePCH.h"
#include "Engine/Renderer/FxaaPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the pipeline of the stub Fxaa program (no bindings), so
// the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the descriptions; Record
// records nothing and returns the source. Stream C replaces the file.

namespace Engine {

	namespace Utils {

		static PipelineLayoutDescription MakeFxaaStubDescription()
		{
			return { .Name = "Fxaa", .Program = "Fxaa", .Entries = { "CSMain" } };
		}

	}

	struct FxaaPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		uint32_t PipelineCount = 0;
	};

	FxaaPass::FxaaPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	FxaaPass::~FxaaPass() = default;

	Result<Scope<FxaaPass>> FxaaPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<FxaaPass> pass = CreateScope<FxaaPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeFxaaStubDescription() }));
		state.PipelineCount = 1;
		return pass;
	}

	std::vector<PipelineLayoutDescription> FxaaPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return { Utils::MakeFxaaStubDescription() };
	}

	uint32_t FxaaPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Result<nvrhi::ITexture*> FxaaPass::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const FxaaPassInputs& inputs)
	{
		ENGINE_CONTRACT_STUB();
		ENGINE_CORE_ASSERT(inputs.Source != nullptr && inputs.Destination != nullptr && inputs.Source != inputs.Destination,
			"FxaaPass::Record needs two different LDR targets");
		// Nothing recorded: the source still holds the image.
		return inputs.Source;
	}

}
