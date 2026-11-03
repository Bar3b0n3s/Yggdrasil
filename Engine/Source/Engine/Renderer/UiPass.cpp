#include "EnginePCH.h"
#include "Engine/Renderer/UiPass.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/TextMeasurement.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The Ui pass (UiPass.h). PlanUiPass walks the draw list once, resolving each sheet, its texture's size and each font once per
// plan, and appends every quad or glyph to the run it extends or to a new one; RecordUiPass records maximal sequences of runs:
// each quad run through one QuadBatcher::Record (its scissor is per record), consecutive glyph runs through one
// TextRenderer::RecordGlyphRuns (a scissor per run).

namespace Engine {

	namespace {

		// A sheet resolved for one plan: Sheet null when it is missing, failed or not a sprite sheet.
		struct PlannedSheet
		{
			AssetHandle Handle{};
			AssetRef<SpriteSheetData> Sheet{};
			glm::uvec2 TextureSize = glm::uvec2(1); // its texture's (or the texture placeholder's); unused for a null texture
		};

		// A font resolved for one plan, as TextRenderer resolves it for the glyph runs (null: the Default font).
		struct PlannedFont
		{
			AssetHandle Handle{};
			AssetRef<FontData> Font{}; // null only when even the placeholder is not a font
		};

		// Reports the sheet `handle` that a UI image cannot draw, as AssetManager::GetOrPlaceholder reports an unavailable
		// asset (§7.2's shared failure policy: ReportDiagnostic records it for the handle and logs it once per manager), but
		// without a placeholder, since there is no placeholder sheet: the image draws nothing. `error` is the load's error,
		// null for an asset of another type. ASSET_MISSING for an unknown handle, ASSET_TYPE_MISMATCH for another type (or a
		// handle that cannot be loaded on its own), ASSET_IMPORT_FAILED for a failed load.
		void ReportUnavailableSheet(AssetManager& assets, AssetHandle handle, const Error* error)
		{
			const std::string_view sheetType = AssetTypeToString(AssetType::SpriteSheet);
			AssetDiagnostic diagnostic{
				.Severity = DiagnosticSeverity::Error,
				.Code = {},
				.Asset = handle,
				.Path = assets.GetReferencePath(handle),
				.Message = {},
				.Hint = {},
				.Subject = {},
				.AutoFixable = false,
			};
			if (error == nullptr || error->GetCode() == ErrorCode::InvalidArgument)
			{
				const AssetType actual = assets.GetAssetType(handle);
				diagnostic.Code = std::string(AssetTypeMismatchCode);
				diagnostic.Message = actual == AssetType::None
					? std::format("asset {} cannot be used as a {}; the UI image draws nothing", handle.ToString(), sheetType)
					: std::format("asset {} is a {}, not a {}; the UI image draws nothing", handle.ToString(), AssetTypeToString(actual), sheetType);
				diagnostic.Hint = std::format("assign an asset of type {}", sheetType);
			}
			else if (error->GetCode() == ErrorCode::NotFound)
			{
				diagnostic.Code = std::string(AssetMissingCode);
				diagnostic.Message = std::format("asset {} is missing; the UI image draws nothing", handle.ToString());
				diagnostic.Hint = "restore the asset, or assign another one";
			}
			else
			{
				diagnostic.Code = std::string(AssetImportFailedCode);
				diagnostic.Message = std::format("asset {} failed to load; the UI image draws nothing: {}", handle.ToString(),
					Error(*error).WithHint(std::string()).ToString()); // the hint is the diagnostic's own member
				diagnostic.Hint = error->GetHint();
			}
			assets.ReportDiagnostic(std::move(diagnostic));
		}

		// What one PlanUiPass resolves, each handle once.
		class UiPlanResolver
		{
		public:
			explicit UiPlanResolver(AssetManager& assets)
				: m_Assets(&assets)
			{
			}

			// The sheet `handle` (valid): there is no placeholder sheet (SpriteSheetData.h), so a missing or failed one, or an
			// asset of another type, gives a null Sheet, reported once per plan (ReportUnavailableSheet).
			[[nodiscard]] const PlannedSheet& FindSheet(AssetHandle handle)
			{
				if (const auto found = std::ranges::find(m_Sheets, handle, &PlannedSheet::Handle); found != m_Sheets.end())
					return *found;
				PlannedSheet& planned = m_Sheets.emplace_back();
				planned.Handle = handle;
				const Result<AssetRef<Asset>> loaded = m_Assets->Load(handle);
				if (loaded.has_value())
					planned.Sheet = AssetCast<SpriteSheetData>(*loaded);
				if (planned.Sheet == nullptr)
					ReportUnavailableSheet(*m_Assets, handle, loaded.has_value() ? nullptr : &loaded.error());
				if (planned.Sheet != nullptr && planned.Sheet->Texture.IsValid())
				{
					// The texture's CPU data gives its size; a missing or failed texture draws its placeholder (as the GPU mirror
					// GpuResourceCache::GetTexture does), so its size is the placeholder's.
					const AssetRef<TextureData> texture = m_Assets->GetOrPlaceholder<TextureData>(planned.Sheet->Texture.GetHandle());
					if (texture != nullptr)
						planned.TextureSize = glm::uvec2(std::max(texture->Width, 1U), std::max(texture->Height, 1U));
				}
				return planned;
			}

			// The font `handle` resolves to: null is the Default font (the font placeholder, without a diagnostic), a missing or
			// failed font its placeholder (reported once by the AssetManager).
			[[nodiscard]] const FontData* FindFont(AssetHandle handle)
			{
				if (const auto found = std::ranges::find(m_Fonts, handle, &PlannedFont::Handle); found != m_Fonts.end())
					return found->Font.get();
				PlannedFont& planned = m_Fonts.emplace_back();
				planned.Handle = handle;
				planned.Font = handle.IsValid() ? m_Assets->GetOrPlaceholder<FontData>(handle) : AssetCast<FontData>(m_Assets->GetPlaceholder(AssetType::Font));
				return planned.Font.get();
			}
		private:
			AssetManager* m_Assets = nullptr; // documented back-reference for the plan's duration
			std::vector<PlannedSheet> m_Sheets{};
			std::vector<PlannedFont> m_Fonts{};
		};

	}

	namespace Utils {

		static bool IsFinite(const glm::vec2& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y);
		}

		static bool IsPositive(float value)
		{
			return std::isfinite(value) && value > 0.0f;
		}

		static RenderTextAlignment ToRenderTextAlignment(UiDrawAlignment alignment)
		{
			switch (alignment)
			{
				case UiDrawAlignment::Start:  return RenderTextAlignment::Left;
				case UiDrawAlignment::Center: return RenderTextAlignment::Center;
				case UiDrawAlignment::End:    return RenderTextAlignment::Right;
			}
			return RenderTextAlignment::Left;
		}

		// The share of the height a text block leaves above it: Start none, Center half, End all.
		static float GetVerticalShare(UiDrawAlignment alignment)
		{
			switch (alignment)
			{
				case UiDrawAlignment::Start:  return 0.0f;
				case UiDrawAlignment::Center: return 0.5f;
				case UiDrawAlignment::End:    return 1.0f;
			}
			return 0.0f;
		}

		static QuadSampler ToQuadSampler(UiDrawFilter filter)
		{
			return filter == UiDrawFilter::Linear ? QuadSampler::Linear : QuadSampler::Point;
		}

		static nvrhi::Rect ToScissor(const UiClipRect& clip)
		{
			return nvrhi::Rect(clip.MinX, clip.MaxX, clip.MinY, clip.MaxY);
		}

		// Appends `count` elements from `first` (the plan's quads or glyphs) to the last run when it continues it with the same
		// kind, clip, font and colour, or to a new run. A quad run keeps a null font and a white colour (the batcher takes
		// each quad's own), and an unclipped run an empty clip.
		static void AppendToRun(UiPassPlan& plan, UiPassRunKind kind, bool clipped, const UiClipRect& clip, AssetHandle font, const glm::vec4& color,
			uint32_t first, uint32_t count)
		{
			const bool glyphs = kind == UiPassRunKind::Glyphs;
			const UiPassRun run{ .Kind = kind,
				.First = first,
				.Count = count,
				.Clipped = clipped,
				.Clip = clipped ? clip : UiClipRect{},
				.Font = glyphs ? font : AssetHandle{},
				.Color = glyphs ? color : glm::vec4(1.0f) };
			if (!plan.Runs.empty())
			{
				UiPassRun& last = plan.Runs.back();
				if (last.Kind == run.Kind && last.Clipped == run.Clipped && last.Clip == run.Clip && last.Font == run.Font && last.Color == run.Color
					&& last.First + last.Count == first)
				{
					last.Count += count;
					return;
				}
			}
			plan.Runs.push_back(run);
		}

		static void AppendQuad(UiPassPlan& plan, const UiPassQuad& quad, bool clipped, const UiClipRect& clip)
		{
			plan.Quads.push_back(quad);
			AppendToRun(plan, UiPassRunKind::Quads, clipped, clip, AssetHandle{}, glm::vec4(1.0f), static_cast<uint32_t>(plan.Quads.size() - 1), 1);
		}

		// Plans an image item (not clipped away); returns whether it drew a quad.
		static bool PlanImage(const UiDrawItem& item, UiPlanResolver& resolver, UiPassPlan& plan)
		{
			UiPassQuad quad{ .Min = item.Min, .Max = item.Max, .Color = item.Color, .Filter = item.Filter };
			const PlannedSheet* sheet = nullptr;
			if (item.Sheet.IsValid())
			{
				sheet = &resolver.FindSheet(item.Sheet);
				if (sheet->Sheet == nullptr || item.Frame >= sheet->Sheet->Frames.size())
				{
					++plan.SkippedItems;
					return false;
				}
			}
			const glm::vec2 size = item.Max - item.Min;
			if (!IsFinite(item.Min) || !IsFinite(item.Max) || !IsPositive(size.x) || !IsPositive(size.y))
				return false;
			if (sheet == nullptr)
			{
				// A plain rectangle of Color: the batcher's white texel.
				AppendQuad(plan, quad, item.Clipped, item.Clip);
				return true;
			}

			const SpriteFrame& frame = sheet->Sheet->Frames[item.Frame];
			quad.Texture = sheet->Sheet->Texture.GetHandle();
			// A sheet without a texture samples white whatever its coordinates; its frames then measure against their own extent.
			const glm::uvec2 textureSize = quad.Texture.IsValid() ? sheet->TextureSize
																  : glm::uvec2(std::max(frame.Rect.X + frame.Rect.Width, 1U), std::max(frame.Rect.Y + frame.Rect.Height, 1U));
			if (item.ImageType == UiDrawImageType::Simple)
			{
				// Texel edges over the texture's size, without a half-texel inset (ExpandNineSlice's coordinates).
				const glm::vec2 texture(static_cast<float>(textureSize.x), static_cast<float>(textureSize.y));
				quad.TexCoordMin = glm::vec2(static_cast<float>(frame.Rect.X), static_cast<float>(frame.Rect.Y)) / texture;
				quad.TexCoordMax = glm::vec2(static_cast<float>(frame.Rect.X + frame.Rect.Width), static_cast<float>(frame.Rect.Y + frame.Rect.Height)) / texture;
				AppendQuad(plan, quad, item.Clipped, item.Clip);
				return true;
			}

			// Sliced. A loaded sheet's borders lie inside their frame (ValidateSpriteSheet); checked here too, since ExpandNineSlice
			// only asserts it.
			const SpriteBorders& borders = frame.Borders;
			const bool bordersFit = static_cast<uint64_t>(borders.Left) + borders.Right <= frame.Rect.Width
				&& static_cast<uint64_t>(borders.Top) + borders.Bottom <= frame.Rect.Height;
			if (!bordersFit || !IsPositive(item.BorderScale))
				return false;
			const NineSliceSource source{ .TextureSize = textureSize,
				.RectMin = glm::uvec2(frame.Rect.X, frame.Rect.Y),
				.RectSize = glm::uvec2(frame.Rect.Width, frame.Rect.Height),
				.Borders = glm::uvec4(borders.Left, borders.Top, borders.Right, borders.Bottom) };
			const NineSliceExpansion expansion = ExpandNineSlice(source, size, item.BorderScale);
			for (uint32_t index = 0; index < expansion.Count; ++index)
			{
				const NineSlicePatch& patch = expansion.Patches[index];
				quad.Min = patch.Min + item.Min;
				quad.Max = patch.Max + item.Min;
				quad.TexCoordMin = patch.TexCoordMin;
				quad.TexCoordMax = patch.TexCoordMax;
				AppendQuad(plan, quad, item.Clipped, item.Clip);
			}
			return expansion.Count > 0;
		}

		// Measures `text` in `font` by `options` and appends its glyphs with the block's top-left corner at `origin` plus the
		// share `verticalShare` of the height `height` leaves, as one glyph run of `fontHandle` and `color`. Returns whether any
		// glyph was appended.
		static bool PlanGlyphs(UiPassPlan& plan, const FontData& font, std::string_view text, const TextMeasureOptions& options, glm::vec2 origin, float height,
			float verticalShare, AssetHandle fontHandle, const glm::vec4& color, bool clipped, const UiClipRect& clip)
		{
			const std::array<const FontData*, 1> chain = { &font };
			const TextMeasurement measured = MeasureText(chain, text, options);
			if (measured.Glyphs.empty())
				return false;
			const glm::vec2 topLeft(origin.x, origin.y + (height - measured.Size.y) * verticalShare);
			const auto first = static_cast<uint32_t>(plan.Glyphs.size());
			for (const TextMeasuredGlyph& glyph : measured.Glyphs)
			{
				plan.Glyphs.push_back(
					TextPixelGlyph{ .Min = glyph.Min + topLeft, .Max = glyph.Max + topLeft, .AtlasMin = glyph.AtlasMin, .AtlasMax = glyph.AtlasMax });
			}
			AppendToRun(plan, UiPassRunKind::Glyphs, clipped, clip, fontHandle, color, first, static_cast<uint32_t>(measured.Glyphs.size()));
			return true;
		}

		// Plans a text item (not clipped away); returns whether it drew a glyph.
		static bool PlanText(const UiDrawItem& item, UiPlanResolver& resolver, UiPassPlan& plan)
		{
			if (item.Text.empty() || !IsFinite(item.Min) || !IsFinite(item.Max) || !IsPositive(item.FontSize) || !IsPositive(item.LineSpacing))
				return false;
			const FontData* font = resolver.FindFont(item.Font);
			if (font == nullptr)
				return false;
			const TextMeasureOptions options{ .Size = item.FontSize,
				.LineSpacing = item.LineSpacing,
				.Wrap = item.Wrap ? TextWrap::Word : TextWrap::None,
				.MaxWidth = item.Max.x - item.Min.x,
				.Alignment = ToRenderTextAlignment(item.HorizontalAlignment) };
			return PlanGlyphs(plan, *font, item.Text, options, item.Min, item.Max.y - item.Min.y, GetVerticalShare(item.VerticalAlignment), item.Font, item.Color,
				item.Clipped, item.Clip);
		}

		// An annotation's one-pixel outline inside its rectangle: its top and bottom rows across its width, then its left and
		// right columns between them, each left out when it has no area (a rectangle under two pixels high has one row).
		static void PlanOutline(const UiAnnotation& annotation, UiPassPlan& plan)
		{
			const glm::vec2 min = annotation.Min;
			const glm::vec2 max = annotation.Max;
			const float topEnd = std::min(min.y + 1.0f, max.y);
			const float bottomStart = std::max(max.y - 1.0f, topEnd);
			const float leftEnd = std::min(min.x + 1.0f, max.x);
			const float rightStart = std::max(max.x - 1.0f, leftEnd);
			const std::array<std::pair<glm::vec2, glm::vec2>, 4> edges = {
				std::pair{ min, glm::vec2(max.x, topEnd) },
				std::pair{ glm::vec2(min.x, bottomStart), max },
				std::pair{ glm::vec2(min.x, topEnd), glm::vec2(leftEnd, bottomStart) },
				std::pair{ glm::vec2(rightStart, topEnd), glm::vec2(max.x, bottomStart) },
			};
			for (const auto& [edgeMin, edgeMax] : edges)
			{
				if (edgeMax.x > edgeMin.x && edgeMax.y > edgeMin.y)
					AppendQuad(plan, UiPassQuad{ .Min = edgeMin, .Max = edgeMax, .Color = annotation.Color }, false, UiClipRect{});
			}
		}

	}

	UiPassPlan PlanUiPass(const UiDrawList& list, AssetManager& assets)
	{
		UiPassPlan plan;
		UiPlanResolver resolver(assets);
		for (const UiDrawItem& item : list.Items)
		{
			// An item whose clipping ancestors leave it no pixel draws nothing.
			if (item.Clipped && item.Clip.IsEmpty())
				continue;
			const bool drew = item.Kind == UiDrawKind::Image ? Utils::PlanImage(item, resolver, plan) : Utils::PlanText(item, resolver, plan);
			plan.DrawnItems += drew ? 1U : 0U;
		}
		for (const UiAnnotation& annotation : list.Annotations)
		{
			if (!Utils::IsFinite(annotation.Min) || !Utils::IsFinite(annotation.Max))
				continue;
			if (annotation.Outline)
				Utils::PlanOutline(annotation, plan);
			if (annotation.Label.empty())
				continue;
			// Left-aligned in the Default font, its block's bottom-left corner at the rectangle's top-left corner.
			const FontData* font = resolver.FindFont(AssetHandle{});
			if (font == nullptr)
				continue;
			const TextMeasureOptions options{ .Size = UiAnnotationLabelSize, .Alignment = RenderTextAlignment::Left };
			Utils::PlanGlyphs(plan, *font, annotation.Label, options, annotation.Min, 0.0f, 1.0f, AssetHandle{}, annotation.Color, false, UiClipRect{});
		}
		return plan;
	}

	Result<uint32_t> RecordUiPass(nvrhi::ICommandList& commandList, PassBindingCache& quadBindings, PassBindingCache& textBindings, const UiPassInputs& inputs,
		RenderPassCounters& counters)
	{
		ENGINE_CORE_ASSERT(inputs.Plan != nullptr && inputs.Quads != nullptr && inputs.Text != nullptr && inputs.Resources != nullptr && inputs.Assets != nullptr
				&& inputs.Framebuffer != nullptr && inputs.ViewConstants != nullptr,
			"RecordUiPass needs a plan, the QuadBatcher, the TextRenderer, the GpuResourceCache, the asset manager, a framebuffer and ViewConstants");
		if (inputs.Plan == nullptr || inputs.Quads == nullptr || inputs.Text == nullptr || inputs.Resources == nullptr || inputs.Assets == nullptr
			|| inputs.Framebuffer == nullptr || inputs.ViewConstants == nullptr)
		{
			return MakeError(ErrorCode::InvalidArgument, "the Ui pass needs a plan, its renderers, its caches, a framebuffer and ViewConstants");
		}
		const UiPassPlan& plan = *inputs.Plan;
		const nvrhi::FramebufferInfoEx& target = inputs.Framebuffer->getFramebufferInfo();
		const glm::mat4 transform = MakeQuadPixelTransform(std::max(target.width, 1U), std::max(target.height, 1U));

		// The textures of this record, each resolved once and held until it ends (a mirror reference is valid only until the
		// cache's next upload, the texture object as long as a handle holds it).
		std::vector<std::pair<AssetHandle, nvrhi::TextureHandle>> textures;
		const auto resolveTexture = [&textures, &inputs](AssetHandle handle) -> nvrhi::ITexture*
		{
			if (const auto found = std::ranges::find(textures, handle, &std::pair<AssetHandle, nvrhi::TextureHandle>::first); found != textures.end())
				return found->second.Get();
			return textures.emplace_back(handle, inputs.Resources->GetTexture(handle).Texture).second.Get();
		};

		Status firstError{};
		const auto keepError = [&firstError](Status status)
		{
			if (!status && firstError)
				firstError = std::move(status);
		};
		std::vector<QuadDraw> quads;
		std::vector<TextGlyphRun> glyphRuns;
		size_t index = 0;
		while (index < plan.Runs.size())
		{
			const UiPassRun& run = plan.Runs[index];
			if (run.Kind == UiPassRunKind::Quads)
			{
				++index;
				const bool inside = static_cast<size_t>(run.First) + run.Count <= plan.Quads.size();
				ENGINE_CORE_ASSERT(inside, "a Ui run's quads [{}, +{}) lie outside the plan's {} quads", run.First, run.Count, plan.Quads.size());
				if (!inside)
					continue;
				quads.clear();
				for (const UiPassQuad& quad : std::span<const UiPassQuad>(plan.Quads).subspan(run.First, run.Count))
				{
					nvrhi::ITexture* texture = nullptr;
					if (quad.Texture.IsValid())
					{
						texture = resolveTexture(quad.Texture);
						// Not even the placeholder's mirror could be created: the quad is skipped rather than drawn white.
						if (texture == nullptr)
							continue;
					}
					quads.push_back(QuadDraw{ .Corners = { glm::vec3(quad.Min.x, quad.Min.y, 0.0f), glm::vec3(quad.Max.x, quad.Min.y, 0.0f),
												  glm::vec3(quad.Max.x, quad.Max.y, 0.0f), glm::vec3(quad.Min.x, quad.Max.y, 0.0f) },
						.TexCoordMin = quad.TexCoordMin,
						.TexCoordMax = quad.TexCoordMax,
						.Color = quad.Color,
						.Texture = texture,
						.Sampler = Utils::ToQuadSampler(quad.Filter) });
				}
				const QuadBatchInputs batch{ .Quads = quads,
					.Target = QuadBatchTarget::Ldr,
					.Framebuffer = inputs.Framebuffer,
					.Scissor = run.Clipped ? std::optional<nvrhi::Rect>(Utils::ToScissor(run.Clip)) : std::nullopt,
					.Transform = transform };
				const Result<uint32_t> recorded = inputs.Quads->Record(commandList, quadBindings, batch, counters);
				if (!recorded)
					keepError(std::unexpected(recorded.error()));
				continue;
			}

			// Consecutive glyph runs record together, each with its own scissor.
			glyphRuns.clear();
			for (; index < plan.Runs.size() && plan.Runs[index].Kind == UiPassRunKind::Glyphs; ++index)
			{
				const UiPassRun& glyphs = plan.Runs[index];
				const bool inside = static_cast<size_t>(glyphs.First) + glyphs.Count <= plan.Glyphs.size();
				ENGINE_CORE_ASSERT(inside, "a Ui run's glyphs [{}, +{}) lie outside the plan's {} glyphs", glyphs.First, glyphs.Count, plan.Glyphs.size());
				if (!inside)
					continue;
				glyphRuns.push_back(TextGlyphRun{ .Font = glyphs.Font,
					.Color = glyphs.Color,
					.FirstGlyph = glyphs.First,
					.GlyphCount = glyphs.Count,
					.Scissor = glyphs.Clipped ? std::optional<nvrhi::Rect>(Utils::ToScissor(glyphs.Clip)) : std::nullopt });
			}
			if (glyphRuns.empty())
				continue;
			const TextGlyphRenderInputs text{ .Glyphs = plan.Glyphs,
				.Runs = glyphRuns,
				.Assets = inputs.Assets,
				.Framebuffer = inputs.Framebuffer,
				.ViewConstants = inputs.ViewConstants };
			const Result<uint32_t> recorded = inputs.Text->RecordGlyphRuns(commandList, textBindings, text, counters);
			if (!recorded)
				keepError(std::unexpected(recorded.error()));
		}
		ENGINE_TRY(firstError);
		return plan.DrawnItems;
	}

	// A stub of the M22 contract (C22.3, Docs/Decisions/0033-m22-contract.md decision 42) that stream F implements.
	std::vector<AssetHandle> BuildUiFontChain(AssetHandle /*font*/, AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
