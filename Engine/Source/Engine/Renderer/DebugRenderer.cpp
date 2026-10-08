#include "EnginePCH.h"
#include "Engine/Renderer/DebugRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"

#include <array>

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the two pipelines of the stub DebugLines program (no
// bindings), so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the
// descriptions; Record draws nothing. Stream D replaces the file.

namespace Engine {

	namespace Utils {

		static std::vector<PipelineLayoutDescription> MakeDebugLinesStubDescriptions()
		{
			return {
				{ .Name = "DebugLinesTested", .Program = "DebugLines", .Entries = { "VSMain", "PSMain" } },
				{ .Name = "DebugLinesOnTop", .Program = "DebugLines", .Entries = { "VSMain", "PSMain" } },
			};
		}

	}

	struct DebugRenderer::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<GraphicsPipeline> Pipelines{};
	};

	DebugLineVertices BuildDebugLineVertices(const DebugDrawList& /*list*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	DebugRenderer::DebugRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	DebugRenderer::~DebugRenderer() = default;

	Result<Scope<DebugRenderer>> DebugRenderer::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<DebugRenderer> renderer = CreateScope<DebugRenderer>(ConstructionKey());
		State& state = *renderer->m_State;
		state.Device = &device;
		for (PipelineLayoutDescription& description : Utils::MakeDebugLinesStubDescriptions())
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(description);
			specification.Primitive = nvrhi::PrimitiveType::LineList;
			specification.Framebuffer = GetOverlayFramebufferInfo();
			ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline, pipelines.CreateGraphicsPipeline(specification));
			state.Pipelines.push_back(std::move(pipeline));
		}
		return renderer;
	}

	std::vector<PipelineLayoutDescription> DebugRenderer::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeDebugLinesStubDescriptions();
	}

	uint32_t DebugRenderer::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<uint32_t> DebugRenderer::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const DebugRenderInputs& /*inputs*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0U;
	}

}
