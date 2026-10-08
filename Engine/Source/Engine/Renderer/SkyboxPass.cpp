#include "EnginePCH.h"
#include "Engine/Renderer/SkyboxPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/ViewConstants.h"

#include <string>
#include <string_view>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SkyboxProgram = "Skybox";
		// The set-0 registers of §8.4 the skybox binds.
		constexpr uint32_t SkyboxViewConstantsSlot = 0;
		constexpr uint32_t SkyboxEnvironmentConstantsSlot = 2;
		constexpr uint32_t SkyboxCubeSlot = 4;
		constexpr uint32_t SkyboxSamplerSlot = 0;

		[[nodiscard]] static PipelineLayoutDescription MakeSkyboxDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Pixel;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(SkyboxViewConstantsSlot),
				nvrhi::BindingLayoutItem::ConstantBuffer(SkyboxEnvironmentConstantsSlot),
				nvrhi::BindingLayoutItem::Texture_SRV(SkyboxCubeSlot),
				nvrhi::BindingLayoutItem::Sampler(SkyboxSamplerSlot),
			};
			return {
				.Name = "Skybox",
				.Program = std::string(SkyboxProgram),
				.Entries = { "VSMain", "PSMain" },
				.Permutation = {},
				.BindingLayouts = { layout },
				.StorageImages = {},
				.ConstantBuffers = {
					{ .Set = 0, .Register = SkyboxViewConstantsSlot, .ByteSize = sizeof(ViewConstants) },
					{ .Set = 0, .Register = SkyboxEnvironmentConstantsSlot, .ByteSize = sizeof(EnvironmentConstants) },
				},
			};
		}

	}

	struct SkyboxPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		GraphicsPipeline Pipeline{};
		nvrhi::SamplerHandle LinearClamp{}; // trilinear, so SkyboxLod blurs between the cube's mips
		uint32_t PipelineCount = 0;
	};

	SkyboxPass::SkyboxPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	SkyboxPass::~SkyboxPass() = default;

	Result<Scope<SkyboxPass>> SkyboxPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<SkyboxPass> pass = CreateScope<SkyboxPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;

		GraphicsPipelineSpecification specification;
		specification.Layout = Utils::MakeSkyboxDescription();
		specification.Framebuffer = GetSceneColorFramebufferInfo();
		specification.Primitive = nvrhi::PrimitiveType::TriangleList;
		// The fullscreen triangle at depth 0: only the pixels the scene left at the cleared depth pass GreaterOrEqual (§8.3,
		// reverse-Z); SceneDepth is read, never written.
		specification.RenderState.rasterState.setCullNone();
		specification.RenderState.rasterState.frontCounterClockwise = true;
		specification.RenderState.depthStencilState.setDepthTestEnable(true).setDepthWriteEnable(false).setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual);
		ENGINE_TRY_ASSIGN(state.Pipeline, pipelines.CreateGraphicsPipeline(specification));
		state.PipelineCount = 1;

		nvrhi::SamplerDesc sampler;
		sampler.setAllFilters(true);
		sampler.setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.LinearClamp, device.CreateSampler(sampler));
		return pass;
	}

	std::vector<PipelineLayoutDescription> SkyboxPass::GetLayoutDescriptions()
	{
		return { Utils::MakeSkyboxDescription() };
	}

	uint32_t SkyboxPass::GetPipelineCount() const
	{
		return m_State->PipelineCount;
	}

	Status SkyboxPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const SkyboxPassInputs& inputs)
	{
		ENGINE_CORE_ASSERT(inputs.Framebuffer != nullptr && inputs.ViewConstants != nullptr && inputs.EnvironmentConstants != nullptr && inputs.Skybox != nullptr,
			"SkyboxPass::Record needs the framebuffer, both constant buffers and the skybox cube");
		ENGINE_CORE_ASSERT(inputs.Skybox->getDesc().dimension == nvrhi::TextureDimension::TextureCube, "SkyboxPass::Record needs a cube texture");
		State& state = *m_State;

		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SkyboxViewConstantsSlot, inputs.ViewConstants),
			nvrhi::BindingSetItem::ConstantBuffer(Utils::SkyboxEnvironmentConstantsSlot, inputs.EnvironmentConstants),
			nvrhi::BindingSetItem::Texture_SRV(Utils::SkyboxCubeSlot, inputs.Skybox),
			nvrhi::BindingSetItem::Sampler(Utils::SkyboxSamplerSlot, state.LinearClamp),
		};
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]));

		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Pipeline.Pipeline;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewportAndScissorRect(inputs.Framebuffer->getFramebufferInfo().getViewport());
		graphics.bindings = { set };

		commandList.beginMarker("Skybox");
		commandList.setGraphicsState(graphics);
		commandList.draw(nvrhi::DrawArguments().setVertexCount(3));
		commandList.endMarker();
		return {};
	}

}
