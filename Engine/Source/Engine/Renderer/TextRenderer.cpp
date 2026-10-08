#include "EnginePCH.h"
#include "Engine/Renderer/TextRenderer.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/SceneTargetFormats.h"

// The contract stub (Docs/Decisions/0013-m8-decisions.md decision 3): the two pipelines of the stub Text program (no
// bindings), so the program is covered by "Shaders: LayoutsMatchReflection" and the pipeline count matches the
// descriptions; Record draws nothing and no atlas is mirrored. Stream D replaces the file.

namespace Engine {

	namespace Utils {

		static std::vector<PipelineLayoutDescription> MakeTextStubDescriptions()
		{
			return {
				{ .Name = "TextTested", .Program = "Text", .Entries = { "VSMain", "PSMain" } },
				{ .Name = "TextOnTop", .Program = "Text", .Entries = { "VSMain", "PSMain" } },
			};
		}

	}

	struct TextRenderer::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<GraphicsPipeline> Pipelines{};
	};

	TextRenderer::TextRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TextRenderer::~TextRenderer() = default;

	Result<Scope<TextRenderer>> TextRenderer::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		ENGINE_CONTRACT_STUB();
		Scope<TextRenderer> renderer = CreateScope<TextRenderer>(ConstructionKey());
		State& state = *renderer->m_State;
		state.Device = &device;
		for (PipelineLayoutDescription& description : Utils::MakeTextStubDescriptions())
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(description);
			specification.Framebuffer = GetOverlayFramebufferInfo();
			ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline, pipelines.CreateGraphicsPipeline(specification));
			state.Pipelines.push_back(std::move(pipeline));
		}
		return renderer;
	}

	std::vector<PipelineLayoutDescription> TextRenderer::GetLayoutDescriptions()
	{
		ENGINE_CONTRACT_STUB();
		return Utils::MakeTextStubDescriptions();
	}

	uint32_t TextRenderer::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<uint32_t> TextRenderer::Record(nvrhi::ICommandList& /*commandList*/, PassBindingCache& /*bindings*/, const TextRenderInputs& /*inputs*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0U;
	}

	void TextRenderer::CollectStale(const AssetManager& /*assets*/, bool /*releaseUnused*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	size_t TextRenderer::GetFontAtlasCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

}
