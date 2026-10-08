#include "EnginePCH.h"
#include "Engine/Renderer/SkyboxPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the pipeline of the stub Skybox program (no bindings),
// so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the descriptions; Record
// draws nothing. Stream B replaces the file.

namespace Engine {

	namespace Utils {

		static PipelineLayoutDescription MakeSkyboxStubDescription()
		{
			return { .Name = "Skybox", .Program = "Skybox", .Entries = { "VSMain", "PSMain" } };
		}

	}

	struct SkyboxPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		GraphicsPipeline Pipeline{};
		uint32_t PipelineCount = 0;
	};

	SkyboxPass::SkyboxPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkyboxPass::~SkyboxPass() = default;

	Result<Scope<SkyboxPass>> SkyboxPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<SkyboxPass> pass = CreateScope<SkyboxPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		GraphicsPipelineSpecification specification;
		specification.Layout = Utils::MakeSkyboxStubDescription();
		specification.Framebuffer = GetSceneColorFramebufferInfo();
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateGraphicsPipeline(specification));
		state.PipelineCount = 1;
		return pass;
	}

	std::vector<PipelineLayoutDescription> SkyboxPass::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return { Utils::MakeSkyboxStubDescription() };
	}

	uint32_t SkyboxPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status SkyboxPass::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const SkyboxPassInputs& /*inputs*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
