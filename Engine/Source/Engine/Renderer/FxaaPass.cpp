#include "EnginePCH.h"
#include "Engine/Renderer/FxaaPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"

// The Fxaa program (Passes/Fxaa.slang): set 0 is t0 the source (read by texel and through s0 LinearClamp), s0 and u0 the
// destination; the shader takes the image size from the destination, so the pass has no constants.

namespace Engine {

	namespace Utils {

		// The Fxaa program's [numthreads(8, 8, 1)].
		constexpr uint32_t FxaaGroupSize = 8;

		static PipelineLayoutDescription MakeFxaaDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Sampler(0),
				nvrhi::BindingLayoutItem::Texture_UAV(0),
			};
			return {
				.Name = "Fxaa",
				.Program = "Fxaa",
				.Entries = { "CSMain" },
				.BindingLayouts = { layout },
				.StorageImages = { { .Set = 0, .Register = 0, .Format = LdrColorFormat } },
			};
		}

	}

	struct FxaaPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
		nvrhi::SamplerHandle LinearClamp{};
		uint32_t PipelineCount = 0;
	};

	FxaaPass::FxaaPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	FxaaPass::~FxaaPass() = default;

	Result<Scope<FxaaPass>> FxaaPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<FxaaPass> pass = CreateScope<FxaaPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateComputePipeline({ .Layout = Utils::MakeFxaaDescription() }));
		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(sampler));
		state.PipelineCount = PipelineCount;
		return pass;
	}

	std::vector<PipelineLayoutDescription> FxaaPass::GetLayoutDescriptions()
	{
		return { Utils::MakeFxaaDescription() };
	}

	uint32_t FxaaPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Result<nvrhi::ITexture*> FxaaPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const FxaaPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.Source != nullptr && inputs.Destination != nullptr && inputs.Source != inputs.Destination,
			"FxaaPass::Record needs two different LDR targets");
		const nvrhi::TextureDesc& target = inputs.Destination->getDesc();
		ENGINE_CORE_ASSERT(inputs.Source->getDesc().width == target.width && inputs.Source->getDesc().height == target.height && target.isUAV,
			"FxaaPass::Record needs a storage destination of the source's size");
		State& state = *m_State;

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, inputs.Source),
			nvrhi::BindingSetItem::Sampler(0, state.LinearClamp),
			nvrhi::BindingSetItem::Texture_UAV(0, inputs.Destination, LdrColorFormat),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]));
		nvrhi::ComputeState compute;
		compute.pipeline = state.Pipeline.Pipeline;
		compute.bindings = { set };

		commandList.beginMarker("FXAA");
		commandList.setComputeState(compute);
		commandList.dispatch((target.width + Utils::FxaaGroupSize - 1) / Utils::FxaaGroupSize, (target.height + Utils::FxaaGroupSize - 1) / Utils::FxaaGroupSize);
		commandList.endMarker();
		return inputs.Destination;
	}

}
