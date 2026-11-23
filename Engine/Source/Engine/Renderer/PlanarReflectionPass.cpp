#include "EnginePCH.h"
#include "Engine/Renderer/PlanarReflectionPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/PlanarReflectionConstants.h"

#include <algorithm>
#include <utility>

namespace Engine {

	struct PlanarReflectionPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		ComputePipeline Pipeline{};
	};

	PlanarReflectionPass::PlanarReflectionPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	PlanarReflectionPass::~PlanarReflectionPass() = default;

	Result<Scope<PlanarReflectionPass>> PlanarReflectionPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<PlanarReflectionPass> pass = CreateScope<PlanarReflectionPass>(ConstructionKey());
		pass->m_State->Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = GetLayoutDescriptions();
		ENGINE_TRY_ASSIGN(pass->m_State->Pipeline, pipelines.CreateComputePipeline({ .Layout = std::move(descriptions.front()) }));
		return pass;
	}

	std::vector<PipelineLayoutDescription> PlanarReflectionPass::GetLayoutDescriptions()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Compute;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = { nvrhi::BindingLayoutItem::PushConstants(0, sizeof(PlanarReflectionMipConstants)), nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Texture_UAV(0) };
		std::vector<PipelineLayoutDescription> descriptions;
		descriptions.push_back({ .Name = "PlanarReflection.CSDownsample",
			.Program = "PlanarReflection",
			.Entries = { "CSDownsample" },
			.BindingLayouts = { layout },
			.StorageImages = { { .Set = 0, .Register = 0, .Format = SceneColorFormat } } });
		return descriptions;
	}

	nvrhi::TextureDesc PlanarReflectionPass::GetColorDesc(uint32_t width, uint32_t height, uint32_t mipCount)
	{
		ENGINE_CORE_ASSERT(width > 0 && height > 0, "a planar reflection's target has a positive size");
		ENGINE_CORE_ASSERT(mipCount > 0 && (std::max(width, height) >> (std::min(mipCount, 32U) - 1)) > 0,
			"a planar reflection's target has from one level to its full chain");
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = height;
		desc.mipLevels = mipCount;
		desc.format = SceneColorFormat;
		desc.dimension = nvrhi::TextureDimension::Texture2D;
		desc.isShaderResource = true;
		desc.isRenderTarget = true;
		desc.isUAV = true;
		desc.initialState = nvrhi::ResourceStates::ShaderResource;
		desc.keepInitialState = true;
		desc.debugName = "SceneRenderer.PlanarReflectionColor";
		return desc;
	}

	Status PlanarReflectionPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, nvrhi::ITexture& color, RenderPassCounters& counters) const
	{
		const State& state = *m_State;
		const nvrhi::TextureDesc& desc = color.getDesc();
		for (uint32_t mip = 1; mip < desc.mipLevels; ++mip)
		{
			PlanarReflectionMipConstants constants;
			constants.SourceSize = glm::uvec2(std::max(1U, desc.width >> (mip - 1)), std::max(1U, desc.height >> (mip - 1)));
			constants.DestinationSize = glm::uvec2(std::max(1U, desc.width >> mip), std::max(1U, desc.height >> mip));
			nvrhi::BindingSetDesc set;
			set.bindings = {
				nvrhi::BindingSetItem::PushConstants(0, sizeof(constants)),
				nvrhi::BindingSetItem::Texture_SRV(0, &color, nvrhi::Format::UNKNOWN, nvrhi::TextureSubresourceSet(mip - 1, 1, 0, 1)),
				nvrhi::BindingSetItem::Texture_UAV(0, &color, SceneColorFormat, nvrhi::TextureSubresourceSet(mip, 1, 0, 1)),
			};
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * created, bindings.GetOrCreate(*state.Device, set, *state.Pipeline.BindingLayouts.front()));
			nvrhi::ComputeState compute;
			compute.pipeline = state.Pipeline.Pipeline;
			compute.bindings = { created };
			commandList.setComputeState(compute);
			commandList.setPushConstants(&constants, sizeof(constants));
			commandList.dispatch((constants.DestinationSize.x + 7) / 8, (constants.DestinationSize.y + 7) / 8);
			++counters.Dispatches;
		}
		return {};
	}

}
