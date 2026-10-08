#include "EnginePCH.h"
#include "Engine/Renderer/BlitPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	namespace Utils {

		constexpr std::string_view BlitProgram = "Blit";

	}

	struct BlitPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		nvrhi::FramebufferInfo Framebuffer{};
		GraphicsPipeline Pipeline{};
		nvrhi::SamplerHandle Sampler{};
		// The binding set of the last source texture, which it keeps alive (so its address cannot be reused while it is
		// cached). A host blits one source, which changes only when its renderer resizes; a new source replaces the set, and
		// NVRHI keeps the previous one until the submissions that used it complete.
		const nvrhi::ITexture* Source = nullptr;
		nvrhi::BindingSetHandle BindingSet{};
	};

	BlitPass::BlitPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	BlitPass::~BlitPass() = default;

	Result<Scope<BlitPass>> BlitPass::Create(GraphicsDevice& device, PipelineFactory& pipelines, const nvrhi::FramebufferInfo& framebuffer)
	{
		const bool hasDepth = framebuffer.depthFormat != nvrhi::Format::UNKNOWN;
		if (hasDepth || framebuffer.colorFormats.size() != 1)
		{
			return MakeError(ErrorCode::InvalidArgument, "a blit needs a framebuffer of one colour attachment and no depth, got {} colour attachment(s){}",
				framebuffer.colorFormats.size(), hasDepth ? " and a depth attachment" : "");
		}

		GraphicsPipelineSpecification specification;
		specification.Layout = GetLayoutDescription();
		// The fullscreen triangle has one winding; nothing is culled. No depth.
		specification.RenderState.rasterState.setCullMode(nvrhi::RasterCullMode::None).setFrontCounterClockwise(true);
		specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
		specification.Framebuffer = framebuffer;
		ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline, pipelines.CreateGraphicsPipeline(specification));

		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(nvrhi::SamplerHandle sampler, device.CreateSampler(samplerDesc));

		Scope<BlitPass> pass = CreateScope<BlitPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		state.Framebuffer = framebuffer;
		state.Pipeline = std::move(pipeline);
		state.Sampler = std::move(sampler);
		return pass;
	}

	Status BlitPass::Record(nvrhi::ICommandList& commandList, nvrhi::ITexture& source, nvrhi::IFramebuffer& framebuffer)
	{
		State& state = *m_State;
		const nvrhi::FramebufferInfoEx& info = framebuffer.getFramebufferInfo();
		ENGINE_CORE_ASSERT(static_cast<const nvrhi::FramebufferInfo&>(info) == state.Framebuffer,
			"BlitPass: the framebuffer does not match the FramebufferInfo the pipeline was created for");

		if (state.Source != &source || state.BindingSet == nullptr)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::Texture_SRV(0, &source),
				nvrhi::BindingSetItem::Sampler(0, state.Sampler),
			};
			ENGINE_TRY_ASSIGN(state.BindingSet, state.Device->CreateBindingSet(desc, *state.Pipeline.BindingLayouts[0]));
			state.Source = &source;
		}

		commandList.beginMarker("Blit");
		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Pipeline.Pipeline;
		graphics.framebuffer = &framebuffer;
		graphics.bindings = { state.BindingSet };
		graphics.viewport.addViewportAndScissorRect(nvrhi::Viewport(static_cast<float>(info.width), static_cast<float>(info.height)));
		commandList.setGraphicsState(graphics);
		commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
		commandList.endMarker();
		return {};
	}

	PipelineLayoutDescription BlitPass::GetLayoutDescription()
	{
		nvrhi::BindingLayoutDesc layout;
		layout.visibility = nvrhi::ShaderType::Pixel;
		layout.registerSpace = 0;
		layout.registerSpaceIsDescriptorSet = true;
		layout.bindings = {
			nvrhi::BindingLayoutItem::Texture_SRV(0),
			nvrhi::BindingLayoutItem::Sampler(0),
		};
		return {
			.Name = "Blit",
			.Program = std::string(Utils::BlitProgram),
			.Entries = { "VSMain", "PSMain" },
			.BindingLayouts = { layout },
		};
	}

}
