#include "EnginePCH.h"
#include "Engine/Renderer/SkyPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/FogConstants.h"
#include "Shared/SkyConstants.h"
#include "Shared/ViewConstants.h"

#include <string>
#include <string_view>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkyProgram = "Sky";
		constexpr uint32_t SkyViewConstantsSlot = 0;
		constexpr uint32_t SkyConstantsSlot = 1;
		constexpr uint32_t SkyFogConstantsSlot = 5;
		constexpr uint32_t SkyTransmittanceSlot = 0;
		constexpr uint32_t SkyViewLutSlot = 2;
		constexpr uint32_t SkyLinearClampSlot = 0;
		constexpr uint32_t SkyLinearWrapUSlot = 1;
		// The push constants' slot: NVRHI counts it among the constant buffers' registers, so not b0 (ViewConstants).
		constexpr uint32_t SkyPushConstantsSlot = 3;

		[[nodiscard]] static PipelineLayoutDescription MakeSkyPassDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			// Pixel for the pass's bindings, and Vertex too for the program's push constants, which both stages list.
			layout.visibility = nvrhi::ShaderType::Vertex | nvrhi::ShaderType::Pixel;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(SkyViewConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SkyConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SkyFogConstantsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SkyTransmittanceSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SkyViewLutSlot),
				nvrhi::BindingLayoutItem::Sampler(SkyLinearClampSlot),
				nvrhi::BindingLayoutItem::Sampler(SkyLinearWrapUSlot),
				// The program's push constants (Passes/Sky.slang's Dispatch), which its reflection lists and the pass does not read.
				nvrhi::BindingLayoutItem::PushConstants(SkyPushConstantsSlot, sizeof(SkyDispatchConstants)),
			};
			return {
				.Name = "Sky",
				.Program = std::string(SkyProgram),
				.Entries = { "VSMain", "PSMain" },
				.Permutation = {},
				.BindingLayouts = { layout },
				.StorageImages = {},
				.ConstantBuffers = {
					{ .Set = 0, .Register = SkyViewConstantsSlot, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = SkyConstantsSlot, .ByteSize = sizeof(SkyConstants) },
					{ .Set = 0, .Register = SkyFogConstantsSlot, .ByteSize = sizeof(FogConstants) },
				},
			};
		}

	}

	struct SkyPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		GraphicsPipeline Pipeline{};
		nvrhi::SamplerHandle LinearClamp{};
		nvrhi::SamplerHandle LinearWrapU{};
		uint32_t PipelineCount = 0;
	};

	SkyPass::SkyPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkyPass::~SkyPass() = default;

	Result<Scope<SkyPass>> SkyPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SkyPass> pass = CreateScope<SkyPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		GraphicsPipelineSpecification specification;
		specification.Layout = Utils::MakeSkyPassDescription();
		specification.Framebuffer = GetSceneColorFramebufferInfo();
		specification.Primitive = nvrhi::PrimitiveType::TriangleList;
		// As the Skybox: only the pixels the scene left at the cleared depth pass GreaterOrEqual; SceneDepth is never written.
		specification.RenderState.rasterState.setCullNone();
		specification.RenderState.rasterState.frontCounterClockwise = true;
		specification.RenderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateGraphicsPipeline(specification));
		state.PipelineCount = 1;
		nvrhi::SamplerDesc clamp;
		clamp.setAllFilters(true);
		clamp.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(clamp));
		nvrhi::SamplerDesc wrap = clamp;
		wrap.setAddressU(nvrhi::SamplerAddressMode::Wrap);
		ENGINE_TRY_ASSIGN(state.LinearWrapU, device.CreateSampler(wrap));
		return pass;
	}

	std::vector<PipelineLayoutDescription> SkyPass::GetLayoutDescriptions()
	{
		return { Utils::MakeSkyPassDescription() };
	}

	uint32_t SkyPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status SkyPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.Framebuffer != nullptr && inputs.ViewConstants != nullptr && inputs.SkyConstants != nullptr && inputs.FogConstants != nullptr
				&& inputs.Transmittance != nullptr && inputs.SkyView != nullptr,
			"SkyPass::Record needs the framebuffer, the three constant buffers and the two LUTs");
		State& state = *m_State;
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SkyViewConstantsSlot, inputs.ViewConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SkyConstantsSlot, inputs.SkyConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SkyFogConstantsSlot, inputs.FogConstants),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SkyTransmittanceSlot, inputs.Transmittance),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SkyViewLutSlot, inputs.SkyView),
			nvrhi::BindingSetItem::Sampler(Utils::SkyLinearClampSlot, state.LinearClamp),
			nvrhi::BindingSetItem::Sampler(Utils::SkyLinearWrapUSlot, state.LinearWrapU),
			nvrhi::BindingSetItem::PushConstants(Utils::SkyPushConstantsSlot, sizeof(SkyDispatchConstants)),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]));
		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Pipeline.Pipeline;
		graphics.framebuffer = inputs.Framebuffer;
		const nvrhi::Viewport viewport = inputs.Framebuffer->getFramebufferInfo().getViewport();
		if (inputs.Scissor)
			graphics.viewport.addViewport(viewport).addScissorRect(*inputs.Scissor);
		else
			graphics.viewport.addViewportAndScissorRect(viewport);
		graphics.bindings = { set };
		commandList.beginMarker("Sky");
		commandList.setGraphicsState(graphics);
		const SkyDispatchConstants unused{};
		commandList.setPushConstants(&unused, sizeof(unused));
		commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
		commandList.endMarker();
		return {};
	}

}
