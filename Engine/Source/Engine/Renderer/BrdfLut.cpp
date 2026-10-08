#include "EnginePCH.h"
#include "Engine/Renderer/BrdfLut.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/BrdfLutConstants.h"

#include <string>
#include <string_view>

// The DFG LUT generator (BrdfLut.h): the BrdfLut program's compute pipeline writes every texel of the RG16_FLOAT texture
// in one dispatch of one command list, executed at once. The texture is a storage image only for that dispatch: it is
// created in ShaderResource with keepInitialState, so the dispatch's command list starts it there, transitions it to
// UnorderedAccess and returns it to ShaderResource, where every later command list finds it.

namespace Engine {

	static_assert(BrdfLut::Size == BrdfLutSize && BrdfLut::LutSampleCount == BrdfLutSampleCount,
		"BrdfLut's constants must equal the generator's (Shared/BrdfLutConstants.h)");

	namespace Utils {

		// The BrdfLut program's [numthreads(8, 8, 1)].
		constexpr uint32_t BrdfLutGroupSize = 8;
		constexpr std::string_view BrdfLutProgram = "BrdfLut";

		static PipelineLayoutDescription MakeBrdfLutDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::Texture_UAV(0) };
			return {
				.Name = "BrdfLut",
				.Program = std::string(BrdfLutProgram),
				.Entries = { "CSMain" },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = BrdfLut::Format } },
			};
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
		Scope<BrdfLut> lut = CreateScope<BrdfLut>(ConstructionKey());
		State& state = *lut->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeBrdfLutDescription() }));
		state.PipelineCount = PipelineCount;

		nvrhi::TextureDesc desc;
		desc.width = Size;
		desc.height = Size;
		desc.format = Format;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.isShaderResource = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "BrdfLut";
		ENGINE_TRY_ASSIGN(nvrhi::TextureHandle texture, device.CreateTexture(desc));

		nvrhi::BindingSetDesc bindings;
		bindings.bindings = { nvrhi::BindingSetItem::Texture_UAV(0, texture, Format) };
		ENGINE_TRY_ASSIGN(const nvrhi::BindingSetHandle set, device.CreateBindingSet(bindings, *state.Pipeline.BindingLayouts[0]));
		// Not an immediate command list: a caller may have the frame's immediate list open (NVRHI's validation allows one).
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
			device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));

		nvrhi::ComputeState compute;
		compute.pipeline = state.Pipeline.Pipeline;
		compute.bindings = { set };
		constexpr uint32_t GroupCount = (Size + Utils::BrdfLutGroupSize - 1) / Utils::BrdfLutGroupSize;
		commandList->open();
		commandList->beginMarker("BrdfLut");
		commandList->setComputeState(compute);
		commandList->dispatch(GroupCount, GroupCount);
		commandList->endMarker();
		commandList->close();
		device.ExecuteCommandList(*commandList);

		state.Texture = std::move(texture);
		return lut;
	}

	std::vector<PipelineLayoutDescription> BrdfLut::GetLayoutDescriptions()
	{
		return { Utils::MakeBrdfLutDescription() };
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
