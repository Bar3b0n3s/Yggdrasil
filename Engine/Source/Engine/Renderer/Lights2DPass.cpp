#include "EnginePCH.h"
#include "Engine/Renderer/Lights2DPass.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/Light2DShading.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/Light2DConstants.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	static_assert(static_cast<uint32_t>(RenderLight2DType::Point) == Light2DTypePoint && static_cast<uint32_t>(RenderLight2DType::Global) == Light2DTypeGlobal,
		"the Lights2D program's light types are RenderLight2DType's values");

	namespace Utils {

		constexpr std::string_view Lights2DProgram = "Lights2D";
		// A Point light's square is two triangles (Passes/Lights2D.slang's SquareCorners); a Global light's is one.
		constexpr uint32_t PointLight2DVertices = 6;
		constexpr uint32_t GlobalLight2DVertices = 3;

		static PipelineLayoutDescription MakeLights2DDescription()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::PushConstants(0, sizeof(Light2DConstants)),
			};
			return {
				.Name = "Lights2D",
				.Program = std::string(Lights2DProgram),
				.Entries = { "VSMain", "PSMain" },
				.BindingLayouts = { layout },
			};
		}

		// The light accumulation: every draw adds its light (colour one, one; the alpha stays 0), double-sided, without depth.
		static GraphicsPipelineSpecification MakeLights2DSpecification()
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = MakeLights2DDescription();
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
			blend.enableBlend()
				.setSrcBlend(nvrhi::BlendFactor::One)
				.setDestBlend(nvrhi::BlendFactor::One)
				.setBlendOp(nvrhi::BlendOp::Add)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::One)
				.setBlendOpAlpha(nvrhi::BlendOp::Add);
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(false).setDepthWriteEnable(false).setStencilEnable(false);
			specification.Framebuffer = GetLights2DLightFramebufferInfo();
			return specification;
		}

		static Light2DConstants MakeLight2DConstants(const Light2DItem& light, const glm::mat4& viewProjection)
		{
			return Light2DConstants{
				.ViewProjection = viewProjection,
				.PositionRadius = glm::vec4(light.Position, light.Radius),
				.RadianceFalloff = glm::vec4(light.Color * light.Intensity, light.Falloff),
				.Height = light.Height,
				.Type = static_cast<uint32_t>(light.Type),
			};
		}

	}

	struct Lights2DPass::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		GraphicsPipeline Pipeline{};
	};

	Lights2DPass::Lights2DPass(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	Lights2DPass::~Lights2DPass() = default;

	Result<Scope<Lights2DPass>> Lights2DPass::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<Lights2DPass> pass = CreateScope<Lights2DPass>(ConstructionKey());
		State& state = *pass->m_State;
		state.Device = &device;
		ENGINE_TRY_ASSIGN(state.Pipeline, WithContext(pipelines.CreateGraphicsPipeline(Utils::MakeLights2DSpecification()), "while creating the Lights2D pipeline"));
		return pass;
	}

	std::vector<PipelineLayoutDescription> Lights2DPass::GetLayoutDescriptions()
	{
		return { Utils::MakeLights2DDescription() };
	}

	uint32_t Lights2DPass::GetPipelineCount() const
	{
		return m_State->Pipeline.Pipeline != nullptr ? 1U : 0U;
	}

	Status Lights2DPass::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const Lights2DPassInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Normals != nullptr && inputs.Light != nullptr, "Lights2DPass::Record needs the normals and the light accumulation");
		if (inputs.Normals == nullptr || inputs.Light == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the Lights2D pass needs the normals and the light accumulation");
		State& state = *m_State;
		nvrhi::BindingSetDesc desc;
		desc.bindings = {
			nvrhi::BindingSetItem::Texture_SRV(0, inputs.Normals),
			nvrhi::BindingSetItem::PushConstants(0, sizeof(Light2DConstants)),
		};
		// Everything that can fail, before the first command.
		ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set,
			WithContext(bindings.GetOrCreate(*state.Device, desc, *state.Pipeline.BindingLayouts[0]), "while recording the 2D lights"));

		commandList.beginMarker("Lights2D");
		nvrhi::ITexture* light = inputs.Light->getDesc().colorAttachments[0].texture;
		commandList.clearTextureFloat(light, nvrhi::AllSubresources, nvrhi::Color(0.0f));
		const nvrhi::FramebufferInfoEx& target = inputs.Light->getFramebufferInfo();
		const uint32_t width = std::max(target.width, 1U);
		const uint32_t height = std::max(target.height, 1U);
		nvrhi::GraphicsState graphics;
		graphics.pipeline = state.Pipeline.Pipeline;
		graphics.framebuffer = inputs.Light;
		graphics.viewport.addViewport(nvrhi::Viewport(static_cast<float>(width), static_cast<float>(height)));
		// The whole target: the pipeline's scissor is dynamic state every draw needs.
		graphics.viewport.addScissorRect(nvrhi::Rect(static_cast<int>(width), static_cast<int>(height)));
		graphics.bindings = { set };
		bool stateSet = false;
		for (const Light2DItem& item : inputs.Lights)
		{
			if (!IsLight2DValid(item))
				continue;
			if (!stateSet)
			{
				stateSet = true;
				commandList.setGraphicsState(graphics);
			}
			const Light2DConstants constants = Utils::MakeLight2DConstants(item, inputs.ViewProjection);
			commandList.setPushConstants(&constants, sizeof(constants));
			const bool global = item.Type == RenderLight2DType::Global;
			commandList.draw(nvrhi::DrawArguments().setVertexCount(global ? Utils::GlobalLight2DVertices : Utils::PointLight2DVertices));
			++counters.DrawCalls;
			counters.Triangles += global ? 1U : 2U;
		}
		commandList.endMarker();
		return {};
	}

}
