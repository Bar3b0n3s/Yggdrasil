#include "EnginePCH.h"
#include "Engine/Renderer/TextRenderer.h"
#include "Engine/Renderer/RenderStats.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/TextLayout.h"
#include "Shared/TextVertex.h"
#include "Shared/ViewConstants.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <compare>
#include <cstddef>
#include <format>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

// Record lays every drawable item out on the CPU into one vertex list (six vertices per glyph quad, Shared/TextVertex.h
// says how the vertex shader places each mode), cutting it into batches wherever the atlas or the pipeline changes, so the
// items keep their order; then it uploads the list once and draws the batches. Font atlases are mirrored per (font handle,
// version); a missing font and a null font share the Default font's mirror, because both resolve to the font placeholder.
// An atlas is uploaded through HostImageUpload, whose command lists are not immediate ones and are executed at once, so it
// can be created while the caller's frame command list is open (NVRHI allows one open immediate list at a time) and a
// mirror is never left empty by a frame list its caller did not execute.

namespace Engine {

	namespace {

		// One glyph vertex of the Text program (Passes/Text.slang VertexInput, in this order).
		struct TextVertex
		{
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec2 Corner = glm::vec2(0.0f);
			glm::vec2 TexCoord = glm::vec2(0.0f);
			glm::vec4 Color = glm::vec4(1.0f);
			uint32_t Mode = TextVertexScreen;
		};

		static_assert(sizeof(TextVertex) == 48, "a text vertex is 12 floats and a uint, tightly packed");

		struct AtlasKey
		{
			AssetHandle Handle{};
			uint64_t Version = 0;

			std::strong_ordering operator<=>(const AtlasKey&) const = default;
			bool operator==(const AtlasKey&) const = default;
		};

		// The mirror of one font version's atlas.
		struct AtlasEntry
		{
			nvrhi::TextureHandle Texture{}; // null when the font has no atlas or its upload failed (its texts are skipped)
			bool IsUsed = false;            // used by a Record since the last CollectStale
		};

		// A font resolved for one Record: the data to lay out with and its atlas mirror.
		struct ResolvedFont
		{
			AssetHandle Requested{};
			AssetRef<FontData> Font{};
			nvrhi::ITexture* Atlas = nullptr; // null: the font's texts are skipped
		};

		// A run of consecutive vertices drawn with one atlas and one pipeline.
		struct TextBatch
		{
			nvrhi::ITexture* Atlas = nullptr;
			size_t Pipeline = 0;
			uint32_t FirstVertex = 0;
			uint32_t VertexCount = 0;
		};

		// Where a glyph corner goes: the vertex's Position and Corner (Shared/TextVertex.h).
		struct PlacedCorner
		{
			glm::vec3 Position = glm::vec3(0.0f);
			glm::vec2 Corner = glm::vec2(0.0f);
		};

	}

	namespace Utils {

		constexpr std::string_view TextProgram = "Text";
		// Indices of the two pipelines (GetLayoutDescriptions' order).
		constexpr size_t TextTestedPipeline = 0;
		constexpr size_t TextOnTopPipeline = 1;
		// The first capacity of the vertex buffer, in vertices; it doubles until a record fits, up to MaxTextVertices.
		constexpr size_t InitialTextVertexCapacity = 6 * 1024;
		// The most vertices one record lays out and draws (TextRenderer.h): 2^20 vertices of 48 bytes, 48 MiB, the vertex
		// buffer's largest size, so text that external input controls (scene files, entity.update, scripts) cannot exhaust
		// CPU or GPU memory, as MaxDebugLineVertices bounds the debug lines.
		constexpr size_t MaxTextVertices = size_t{ 1 } << 20;
		constexpr size_t VerticesPerQuad = 6;
		// A glyph quad's two triangles (top left, top right, bottom right; top left, bottom right, bottom left), as indices of
		// its corners in that order.
		constexpr std::array<size_t, 6> QuadCornerOrder = { 0, 1, 2, 0, 2, 3 };

		static PipelineLayoutDescription MakeTextDescription(std::string name)
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::All;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = {
				nvrhi::BindingLayoutItem::ConstantBuffer(0),
				nvrhi::BindingLayoutItem::Texture_SRV(0),
				nvrhi::BindingLayoutItem::Sampler(0),
			};
			return {
				.Name = std::move(name),
				.Program = std::string(TextProgram),
				.Entries = { "VSMain", "PSMain" },
				.BindingLayouts = { layout },
				.ConstantBuffers = { { .Set = 0, .Register = 0, .ByteSize = sizeof(ViewConstants) } },
			};
		}

		static std::vector<PipelineLayoutDescription> MakeTextDescriptions()
		{
			return { MakeTextDescription("TextTested"), MakeTextDescription("TextOnTop") };
		}

		static std::vector<nvrhi::VertexAttributeDesc> MakeTextVertexAttributes()
		{
			constexpr uint32_t Stride = sizeof(TextVertex);
			const auto attribute = [](const char* name, nvrhi::Format format, size_t offset)
			{
				return nvrhi::VertexAttributeDesc().setName(name).setFormat(format).setOffset(static_cast<uint32_t>(offset)).setElementStride(Stride);
			};
			return {
				attribute("POSITION", nvrhi::Format::RGB32_FLOAT, offsetof(TextVertex, Position)),
				attribute("CORNER", nvrhi::Format::RG32_FLOAT, offsetof(TextVertex, Corner)),
				attribute("TEXCOORD", nvrhi::Format::RG32_FLOAT, offsetof(TextVertex, TexCoord)),
				attribute("COLOR", nvrhi::Format::RGBA32_FLOAT, offsetof(TextVertex, Color)),
				attribute("MODE", nvrhi::Format::R32_UINT, offsetof(TextVertex, Mode)),
			};
		}

		// A text pipeline: double-sided glyph quads with straight alpha into the overlay framebuffer, depth-tested against
		// SceneDepth (reverse-Z, GreaterOrEqual, no writes) or on top.
		static GraphicsPipelineSpecification MakeTextSpecification(PipelineLayoutDescription layout, bool depthTested,
			const std::vector<nvrhi::BindingLayoutHandle>& sharedLayouts)
		{
			GraphicsPipelineSpecification specification;
			specification.Layout = std::move(layout);
			specification.SharedBindingLayouts = sharedLayouts;
			specification.VertexAttributes = MakeTextVertexAttributes();
			specification.Primitive = nvrhi::PrimitiveType::TriangleList;
			nvrhi::BlendState::RenderTarget& blend = specification.RenderState.blendState.targets[0];
			blend.enableBlend()
				.setSrcBlend(nvrhi::BlendFactor::SrcAlpha)
				.setDestBlend(nvrhi::BlendFactor::InvSrcAlpha)
				.setBlendOp(nvrhi::BlendOp::Add)
				.setSrcBlendAlpha(nvrhi::BlendFactor::One)
				.setDestBlendAlpha(nvrhi::BlendFactor::InvSrcAlpha)
				.setBlendOpAlpha(nvrhi::BlendOp::Add);
			specification.RenderState.rasterState.setCullNone().enableDepthClip();
			specification.RenderState.depthStencilState.setDepthTestEnable(depthTested)
				.setDepthWriteEnable(false)
				.setDepthFunc(nvrhi::ComparisonFunc::GreaterOrEqual)
				.setStencilEnable(false);
			specification.Framebuffer = GetOverlayFramebufferInfo();
			return specification;
		}

		static bool IsFinite(const glm::vec2& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y);
		}

		static bool IsFinite(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		static bool IsFinite(const glm::vec4& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z) && std::isfinite(vector.w);
		}

		static bool IsFinite(const glm::mat4& matrix)
		{
			return IsFinite(matrix[0]) && IsFinite(matrix[1]) && IsFinite(matrix[2]) && IsFinite(matrix[3]);
		}

		static bool IsValidSize(float size)
		{
			return std::isfinite(size) && size > 0.0f;
		}

		// Whether every vertex's Position and Corner is finite: finite inputs can still place a glyph beyond float's range (a
		// huge Size, a long line), and such a vertex never reaches the GPU.
		static bool HasFinitePlacement(std::span<const TextVertex> vertices)
		{
			return std::ranges::all_of(vertices, [](const TextVertex& vertex)
			{
				return IsFinite(vertex.Position) && IsFinite(vertex.Corner);
			});
		}

		// Whether a text item has the values its space reads, all finite, and something to draw.
		static bool IsDrawable(const TextItem& item)
		{
			if (item.Text.empty() || !IsValidSize(item.Size) || !IsFinite(item.Color) || !IsFinite(item.Pivot))
				return false;
			if (item.Space == RenderTextSpace::World)
				return IsFinite(item.World);
			return IsFinite(item.Anchor) && IsFinite(item.Offset);
		}

		// The readable name of `handle` for debug names and diagnostics: its reference path, or its 16 hex digits.
		static std::string GetFontName(const AssetManager& assets, AssetHandle handle)
		{
			std::string path = assets.GetReferencePath(handle);
			return path.empty() ? handle.ToString() : path;
		}

		// An immutable R8_UNORM texture holding `font`'s atlas, through HostImageUpload (see the file comment).
		static Result<nvrhi::TextureHandle> UploadAtlas(GraphicsDevice& device, const FontData& font, const std::string& name)
		{
			nvrhi::TextureDesc desc;
			desc.width = font.AtlasWidth;
			desc.height = font.AtlasHeight;
			desc.format = nvrhi::Format::R8_UNORM;
			desc.dimension = nvrhi::TextureDimension::Texture2D;
			desc.debugName = std::format("{} (atlas)", name);
			const std::array<TextureSubresourceData, 1> subresources = { TextureSubresourceData{
				.MipLevel = 0,
				.ArraySlice = 0,
				.Data = std::span<const std::byte>(font.AtlasPixels),
				.RowPitch = 0,
				.DepthPitch = 0,
			} };
			// HostImageUpload's command lists are not immediate ones, so the upload works while the caller's frame list is open.
			ENGINE_TRY_ASSIGN(TextureUpload upload, device.GetHostImageUpload().CreateTexture(desc, subresources));
			return std::move(upload.Texture);
		}

		// Appends the six vertices of each quad of `layout`, each corner placed by `place` (block em units -> PlacedCorner).
		template<typename Place>
		static void AppendQuads(std::vector<TextVertex>& vertices, const TextLayoutResult& layout, const glm::vec4& color, uint32_t mode,
			const Place& place)
		{
			for (const TextGlyphQuad& quad : layout.Quads)
			{
				const std::array<glm::vec2, 4> corners = {
					quad.PositionMin,
					glm::vec2(quad.PositionMax.x, quad.PositionMin.y),
					quad.PositionMax,
					glm::vec2(quad.PositionMin.x, quad.PositionMax.y),
				};
				const std::array<glm::vec2, 4> texCoords = {
					quad.AtlasMin,
					glm::vec2(quad.AtlasMax.x, quad.AtlasMin.y),
					quad.AtlasMax,
					glm::vec2(quad.AtlasMin.x, quad.AtlasMax.y),
				};
				for (const size_t corner : QuadCornerOrder)
				{
					const PlacedCorner placed = place(corners[corner]);
					vertices.push_back({ .Position = placed.Position, .Corner = placed.Corner, .TexCoord = texCoords[corner], .Color = color, .Mode = mode });
				}
			}
		}

	}

	struct TextRenderer::State
	{
		GraphicsDevice* Device = nullptr; // documented back-reference
		std::vector<GraphicsPipeline> Pipelines{};
		nvrhi::SamplerHandle Sampler{};
		nvrhi::BufferHandle Vertices{};
		size_t VertexCapacity = 0;
		std::map<AtlasKey, AtlasEntry> Atlases{};
		bool HasWarnedBudget = false; // the vertex-budget warning is logged once per renderer
		// Reused by every Record.
		std::vector<TextVertex> Staging{};
		std::vector<TextBatch> Batches{};
		std::vector<ResolvedFont> Fonts{};

		// The font `handle` resolves to (null: the Default font) with its atlas mirror, uploading the atlas when this version
		// has none yet; each handle is resolved once per Record.
		[[nodiscard]] const ResolvedFont& ResolveFont(AssetManager& assets, AssetHandle handle);
		// Batches the vertices added to Staging since `firstVertex`: they extend the last batch when it has the same atlas and
		// pipeline, and start a new one otherwise.
		void AddBatch(nvrhi::ITexture* atlas, size_t pipeline, size_t firstVertex);
	};

	const ResolvedFont& TextRenderer::State::ResolveFont(AssetManager& assets, AssetHandle handle)
	{
		if (const auto found = std::ranges::find(Fonts, handle, &ResolvedFont::Requested); found != Fonts.end())
			return *found;

		// The font placeholder is the Default font when it loads (AssetManager::GetPlaceholder); a null handle is the
		// placeholder without a diagnostic, a missing or failed font the placeholder with one (GetOrPlaceholder). The
		// placeholder is asked for only when it is used, so a font that loads never needs the Default font (whose absence
		// GetPlaceholder reports as an error).
		ResolvedFont& resolved = Fonts.emplace_back();
		resolved.Requested = handle;
		if (handle.IsValid())
		{
			if (const Result<AssetRef<Asset>> loaded = assets.Load(handle); loaded.has_value())
				resolved.Font = AssetCast<FontData>(*loaded);
		}
		const bool isPlaceholder = resolved.Font == nullptr;
		if (isPlaceholder)
		{
			// The manager remembers the failed load, so GetOrPlaceholder only reports it (once) and serves the placeholder.
			resolved.Font = handle.IsValid() ? assets.GetOrPlaceholder<FontData>(handle) : AssetCast<FontData>(assets.GetPlaceholder(AssetType::Font));
		}
		if (resolved.Font == nullptr)
			return resolved;
		const AssetHandle mirrored = isPlaceholder ? GetPlaceholderHandle(AssetType::Font) : handle;
		const AtlasKey key = { .Handle = mirrored, .Version = assets.GetVersion(mirrored) };
		auto entry = Atlases.find(key);
		if (entry == Atlases.end())
		{
			AtlasEntry created;
			const FontData& font = *resolved.Font;
			if (font.AtlasWidth > 0 && font.AtlasHeight > 0 && !font.Glyphs.empty())
			{
				const std::string name = Utils::GetFontName(assets, mirrored);
				Result<nvrhi::TextureHandle> uploaded = Utils::UploadAtlas(*Device, font, name);
				if (uploaded.has_value())
				{
					created.Texture = std::move(*uploaded);
				}
				else
				{
					// Reported once per font version: the failed entry stays until the font changes or goes unused (§8.14 item 7).
					AssetDiagnostic diagnostic;
					diagnostic.Severity = DiagnosticSeverity::Error;
					diagnostic.Code = std::string(AssetUploadFailedCode);
					diagnostic.Asset = mirrored;
					diagnostic.Path = name.substr(0, name.find('#'));
					diagnostic.Message = std::format("cannot upload the atlas of font '{}' to the GPU: {}; its texts are not drawn", name, uploaded.error().ToString());
					diagnostic.Hint = "the upload is tried again when the font changes; when the device is out of memory, use fewer or smaller "
									  "fonts and textures";
					assets.ReportDiagnostic(std::move(diagnostic));
				}
			}
			entry = Atlases.insert_or_assign(key, std::move(created)).first;
		}
		entry->second.IsUsed = true;
		resolved.Atlas = entry->second.Texture.Get();
		return resolved;
	}

	void TextRenderer::State::AddBatch(nvrhi::ITexture* atlas, size_t pipeline, size_t firstVertex)
	{
		const uint32_t count = static_cast<uint32_t>(Staging.size() - firstVertex);
		if (count == 0)
			return;
		if (!Batches.empty() && Batches.back().Atlas == atlas && Batches.back().Pipeline == pipeline)
		{
			Batches.back().VertexCount += count;
			return;
		}
		Batches.push_back({ .Atlas = atlas, .Pipeline = pipeline, .FirstVertex = static_cast<uint32_t>(firstVertex), .VertexCount = count });
	}

	TextRenderer::TextRenderer(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	TextRenderer::~TextRenderer() = default;

	Result<Scope<TextRenderer>> TextRenderer::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		Scope<TextRenderer> renderer = CreateScope<TextRenderer>(ConstructionKey());
		State& state = *renderer->m_State;
		state.Device = &device;
		std::vector<PipelineLayoutDescription> descriptions = Utils::MakeTextDescriptions();
		// Both pipelines share one binding layout, so one binding set per atlas and view serves both.
		ENGINE_TRY_ASSIGN(const std::vector<nvrhi::BindingLayoutHandle> sharedLayouts,
			WithContext(pipelines.CreateBindingLayouts(descriptions[Utils::TextTestedPipeline]), "while creating the text pipelines"));
		for (size_t index = 0; index < descriptions.size(); ++index)
		{
			const bool depthTested = index == Utils::TextTestedPipeline;
			ENGINE_TRY_ASSIGN(GraphicsPipeline pipeline,
				WithContext(pipelines.CreateGraphicsPipeline(Utils::MakeTextSpecification(std::move(descriptions[index]), depthTested, sharedLayouts)),
					"while creating the text pipelines"));
			state.Pipelines.push_back(std::move(pipeline));
		}
		nvrhi::SamplerDesc samplerDesc;
		samplerDesc.setAllFilters(true).setAllAddressModes(nvrhi::SamplerAddressMode::Clamp);
		ENGINE_TRY_ASSIGN(state.Sampler, WithContext(device.CreateSampler(samplerDesc), "while creating the text renderer"));
		return renderer;
	}

	std::vector<PipelineLayoutDescription> TextRenderer::GetLayoutDescriptions()
	{
		return Utils::MakeTextDescriptions();
	}

	uint32_t TextRenderer::GetPipelineCount() const
	{
		return static_cast<uint32_t>(m_State->Pipelines.size());
	}

	Result<uint32_t> TextRenderer::Record(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TextRenderInputs& inputs)
	{
		RenderPassCounters counters{};
		return RecordCounted(commandList, bindings, inputs, counters);
	}

	Result<uint32_t> TextRenderer::RecordCounted(nvrhi::ICommandList& commandList, PassBindingCache& bindings, const TextRenderInputs& inputs, RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Assets != nullptr && inputs.Framebuffer != nullptr && inputs.ViewConstants != nullptr,
			"TextRenderer::Record needs an asset manager, a framebuffer and ViewConstants");
		State& state = *m_State;
		state.Staging.clear();
		state.Batches.clear();
		state.Fonts.clear();

		const nvrhi::FramebufferInfoEx& target = inputs.Framebuffer->getFramebufferInfo();
		const uint32_t width = std::max(target.width, 1U);
		const uint32_t height = std::max(target.height, 1U);
		const glm::vec2 viewport(static_cast<float>(width), static_cast<float>(height));
		// Pixels per em per unit of Size (§8.10: sizes are given at the 1080p reference height), and pixels to normalized
		// device coordinates (+y up).
		const float pixelScale = viewport.y / TextReferenceHeight;
		const glm::vec2 pixelToNdc = glm::vec2(2.0f, -2.0f) / viewport;
		AssetManager& assets = *inputs.Assets;
		uint32_t drawn = 0;
		uint32_t budgetSkipped = 0; // items left out because their quads exceed the vertex budget

		// Lays out `text`, appends its quads through `place` and batches them; returns whether any quad was added. An item
		// whose quads do not fit in what remains of MaxTextVertices is left out whole and counted in `budgetSkipped` (a later,
		// smaller one may still fit); one with a vertex placed beyond float's range is left out.
		const auto addText = [&state, &assets, &budgetSkipped](AssetHandle fontHandle, std::string_view text, RenderTextAlignment alignment,
								 const glm::vec4& color, uint32_t mode, size_t pipeline, const auto& place) -> bool
		{
			const ResolvedFont& font = state.ResolveFont(assets, fontHandle);
			if (font.Atlas == nullptr)
				return false;
			const TextLayoutResult layout = LayoutText(*font.Font, text, alignment);
			if (layout.Quads.empty())
				return false;
			if (layout.Quads.size() > (Utils::MaxTextVertices - state.Staging.size()) / Utils::VerticesPerQuad)
			{
				++budgetSkipped;
				return false;
			}
			const size_t firstVertex = state.Staging.size();
			// The block's size is the second argument of `place`.
			Utils::AppendQuads(state.Staging, layout, color, mode, [&place, &layout](const glm::vec2& corner)
			{
				return place(corner, layout.Size);
			});
			if (!Utils::HasFinitePlacement(std::span<const TextVertex>(state.Staging).subspan(firstVertex)))
			{
				state.Staging.resize(firstVertex);
				return false;
			}
			state.AddBatch(font.Atlas, pipeline, firstVertex);
			return true;
		};

		// 1. World texts, depth-tested: an em of Size / WorldTextPixelsPerMetre metres in the entity's local XY plane, the
		// block's Pivot at its origin.
		if (inputs.HasCamera)
		{
			for (const TextItem& item : inputs.Texts)
			{
				if (item.Space != RenderTextSpace::World || !Utils::IsDrawable(item))
					continue;
				const float metresPerEm = item.Size / WorldTextPixelsPerMetre;
				const glm::vec2 pivot = item.Pivot;
				const auto local = [metresPerEm, pivot](const glm::vec2& corner, const glm::vec2& block)
				{
					return glm::vec2(corner.x - pivot.x * block.x, pivot.y * block.y - corner.y) * metresPerEm;
				};
				bool added = false;
				if (item.Billboard)
				{
					// The entity's position and scale; the camera's orientation (the vertex shader adds the offset in view space).
					const glm::vec3 origin(item.World[3]);
					const glm::vec2 scale(glm::length(glm::vec3(item.World[0])), glm::length(glm::vec3(item.World[1])));
					added = addText(item.Font, item.Text, item.Alignment, item.Color, TextVertexBillboard, Utils::TextTestedPipeline,
						[&local, origin, scale](const glm::vec2& corner, const glm::vec2& block)
					{
						return PlacedCorner{ .Position = origin, .Corner = local(corner, block) * scale };
					});
				}
				else
				{
					const glm::mat4& world = item.World;
					added = addText(item.Font, item.Text, item.Alignment, item.Color, TextVertexWorld, Utils::TextTestedPipeline,
						[&local, &world](const glm::vec2& corner, const glm::vec2& block)
					{
						return PlacedCorner{ .Position = glm::vec3(world * glm::vec4(local(corner, block), 0.0f, 1.0f)), .Corner = glm::vec2(0.0f) };
					});
				}
				drawn += added ? 1U : 0U;
			}
		}

		// 2. Screen texts, on top: Size and Offset scaled by the viewport height / TextReferenceHeight, placed by
		// PlaceScreenText.
		for (const TextItem& item : inputs.Texts)
		{
			if (item.Space != RenderTextSpace::Screen || !Utils::IsDrawable(item))
				continue;
			const float pixelsPerEm = item.Size * pixelScale;
			const bool added = addText(item.Font, item.Text, item.Alignment, item.Color, TextVertexScreen, Utils::TextOnTopPipeline,
				[&item, pixelsPerEm, width, height, pixelToNdc](const glm::vec2& corner, const glm::vec2& block)
			{
				const glm::vec2 topLeft = PlaceScreenText(item, block * pixelsPerEm, width, height);
				const glm::vec2 ndc = (topLeft + corner * pixelsPerEm) * pixelToNdc + glm::vec2(-1.0f, 1.0f);
				return PlacedCorner{ .Position = glm::vec3(ndc, 0.0f), .Corner = glm::vec2(0.0f) };
			});
			drawn += added ? 1U : 0U;
		}

		// 3. The debug list's labels, in the Default font: centred on the projected Position, Size pixels per em at the 1080p
		// reference, depth-tested or on top by their mode.
		if (inputs.HasCamera && inputs.DebugDraw != nullptr)
		{
			for (const DebugDrawCommand& command : inputs.DebugDraw->GetCommands())
			{
				const DebugText* label = std::get_if<DebugText>(&command.Shape);
				if (label == nullptr || label->Text.empty() || !Utils::IsValidSize(label->Size) || !Utils::IsFinite(label->Position)
					|| !Utils::IsFinite(command.Color))
				{
					continue;
				}
				// On or behind the camera plane: never visible (the vertex shader applies the same test without CameraView).
				if (inputs.CameraView.has_value() && ((*inputs.CameraView) * glm::vec4(label->Position, 1.0f)).z >= 0.0f)
					continue;
				const float pixelsPerEm = label->Size * pixelScale;
				const glm::vec3 position = label->Position;
				const size_t pipeline = command.Depth == DebugDepthMode::OnTop ? Utils::TextOnTopPipeline : Utils::TextTestedPipeline;
				const bool added = addText(AssetHandle(), label->Text, RenderTextAlignment::Center, command.Color, TextVertexLabel, pipeline,
					[position, pixelsPerEm, pixelToNdc](const glm::vec2& corner, const glm::vec2& block)
				{
					return PlacedCorner{ .Position = position, .Corner = (corner - block * 0.5f) * pixelsPerEm * pixelToNdc };
				});
				drawn += added ? 1U : 0U;
			}
		}

		if (budgetSkipped > 0 && !state.HasWarnedBudget)
		{
			state.HasWarnedBudget = true;
			ENGINE_CORE_WARN("The text renderer left out {} text(s) whose glyphs exceed the budget of {} vertices per view; further texts "
							 "beyond the budget are left out without a warning",
				budgetSkipped, Utils::MaxTextVertices);
		}
		if (state.Staging.empty())
			return drawn;

		// Everything that can fail, before the first command. The list never exceeds MaxTextVertices, so neither does the
		// buffer.
		const size_t vertexCount = state.Staging.size();
		if (state.Vertices == nullptr || state.VertexCapacity < vertexCount)
		{
			size_t capacity = std::max(state.VertexCapacity, Utils::InitialTextVertexCapacity);
			while (capacity < vertexCount)
				capacity *= 2;
			capacity = std::min(capacity, Utils::MaxTextVertices);
			nvrhi::BufferDesc desc;
			desc.byteSize = static_cast<uint64_t>(capacity) * sizeof(TextVertex);
			desc.isVertexBuffer = true;
			desc.initialState = nvrhi::ResourceStates::VertexBuffer;
			desc.keepInitialState = true;
			desc.debugName = "TextRenderer.Vertices";
			ENGINE_TRY_ASSIGN(nvrhi::BufferHandle buffer, WithContext(state.Device->CreateBuffer(desc), "while recording the text"));
			state.Vertices = std::move(buffer);
			state.VertexCapacity = capacity;
		}
		nvrhi::IBindingLayout& layout = *state.Pipelines[Utils::TextTestedPipeline].BindingLayouts[0];
		std::vector<nvrhi::IBindingSet*> sets;
		sets.reserve(state.Batches.size());
		for (const TextBatch& batch : state.Batches)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = {
				nvrhi::BindingSetItem::ConstantBuffer(0, inputs.ViewConstants),
				nvrhi::BindingSetItem::Texture_SRV(0, batch.Atlas),
				nvrhi::BindingSetItem::Sampler(0, state.Sampler),
			};
			ENGINE_TRY_ASSIGN(nvrhi::IBindingSet * set, WithContext(bindings.GetOrCreate(*state.Device, desc, layout), "while recording the text"));
			sets.push_back(set);
		}

		commandList.beginMarker("Text");
		commandList.writeBuffer(state.Vertices, state.Staging.data(), vertexCount * sizeof(TextVertex), 0);
		nvrhi::GraphicsState graphics;
		graphics.framebuffer = inputs.Framebuffer;
		graphics.viewport.addViewportAndScissorRect(nvrhi::Viewport(viewport.x, viewport.y));
		graphics.bindings.push_back(nullptr);
		graphics.addVertexBuffer(nvrhi::VertexBufferBinding().setBuffer(state.Vertices).setSlot(0).setOffset(0));
		for (size_t index = 0; index < state.Batches.size(); ++index)
		{
			const TextBatch& batch = state.Batches[index];
			nvrhi::IGraphicsPipeline* pipeline = state.Pipelines[batch.Pipeline].Pipeline;
			if (index == 0 || graphics.pipeline != pipeline || graphics.bindings[0] != sets[index])
			{
				graphics.pipeline = pipeline;
				graphics.bindings[0] = sets[index];
				commandList.setGraphicsState(graphics);
			}
			nvrhi::DrawArguments arguments;
			arguments.vertexCount = batch.VertexCount;
			arguments.startVertexLocation = batch.FirstVertex;
			commandList.draw(arguments);
			++counters.DrawCalls;
			counters.Triangles += batch.VertexCount / 3;
		}
		commandList.endMarker();
		return drawn;
	}

	void TextRenderer::CollectStale(const AssetManager& assets, bool releaseUnused)
	{
		std::erase_if(m_State->Atlases, [&assets, releaseUnused](const auto& item)
		{
			return assets.GetVersion(item.first.Handle) != item.first.Version || (releaseUnused && !item.second.IsUsed);
		});
		for (auto& [key, entry] : m_State->Atlases)
			entry.IsUsed = false;
	}

	size_t TextRenderer::GetFontAtlasCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(m_State->Atlases, [](const auto& item)
		{
			return item.second.Texture != nullptr;
		}));
	}

}
