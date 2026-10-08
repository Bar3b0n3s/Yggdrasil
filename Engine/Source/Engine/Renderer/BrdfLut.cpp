#include "EnginePCH.h"
#include "Engine/Renderer/BrdfLut.h"

#include "Engine/Graphics/GraphicsDevice.h"

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the pipeline of the stub BrdfLut program (no
// bindings), so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the
// descriptions; no LUT is generated (GetTexture is null). Stream A replaces the file.

namespace Engine {

	namespace Utils {

		static PipelineLayoutDescription MakeBrdfLutStubDescription()
		{
			return { .Name = "BrdfLut", .Program = "BrdfLut", .Entries = { "CSMain" } };
		}

	}

	struct BrdfLut::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		nvrhi::TextureHandle Texture{};
		uint32_t PipelineCount = 0;
	};

	BrdfLut::BrdfLut(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	BrdfLut::~BrdfLut() = default;

	Result<Scope<BrdfLut>> BrdfLut::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<BrdfLut> lut = CreateScope<BrdfLut>(ConstructionKey());
		State& state = *lut->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeBrdfLutStubDescription() }));
		state.PipelineCount = 1;
		return lut;
	}

	std::vector<PipelineLayoutDescription> BrdfLut::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return { Utils::MakeBrdfLutStubDescription() };
	}

	uint32_t BrdfLut::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	nvrhi::ITexture* BrdfLut::GetTexture() const
	{
		return m_State->Texture.Get();
	}

}
