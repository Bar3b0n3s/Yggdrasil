#include "EnginePCH.h"
#include "Engine/Renderer/UiPass.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
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
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Ui pass (UiPass.h). PlanUiPass walks the draw list once, resolving each sheet, its texture's size, each font and each
// text font's chain (BuildUiFontChain) once per plan, and appends every quad or glyph to the run it extends or to a new one;
// RecordUiPass records maximal sequences of runs: each quad run through one QuadBatcher::Record (its scissor is per record),
// consecutive glyph runs through one TextRenderer::RecordGlyphRuns (a scissor per run). C22.3: a World canvas's item is
// planned alone in its canvas pixels and its quads and glyphs cut to its clip on the CPU into runs of its canvas, which
// RecordUiPass records first, back to front, placed in the world by the canvas's matrix and depth-tested.

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

		// The font chain of a text in the font Font, resolved for one plan (BuildUiFontChain): Fonts measures the text, and
		// RunFonts[i] is the font a glyph run of Fonts[i] names (the text's own font as the item names it, null for the Default
		// font as before C22.3, then the fallbacks). Both empty when not even the placeholder is a font. Fonts borrows the
		// fonts the resolver's m_Fonts holds (FindFont): each PlannedFont's AssetRef keeps its font alive, and m_Fonts never
		// erases an entry during the plan (a reallocation moves the references, not the fonts), so the pointers stay valid
		// while the resolver lives.
		struct PlannedChain
		{
			AssetHandle Font{};
			std::vector<AssetHandle> RunFonts{};
			std::vector<const FontData*> Fonts{};
		};

		// Reports the asset `handle` of type `type` that the Ui pass cannot use, as AssetManager::GetOrPlaceholder reports an
		// unavailable asset (§7.2's shared failure policy: ReportDiagnostic records it for the handle and logs it once per
		// manager), but without a placeholder: a UI image's sheet (there is no placeholder sheet: the image draws nothing) or a
		// fallback of a text's font chain (left out: a placeholder has every glyph a fallback is meant for). `consequence`
		// says what follows ("the UI image draws nothing"). `error` is the load's error, null for an asset of another type.
		// ASSET_MISSING for an unknown handle, ASSET_TYPE_MISMATCH for another type (or a handle that cannot be loaded on its
		// own), ASSET_IMPORT_FAILED for a failed load.
		void ReportUnavailableAsset(AssetManager& assets, AssetHandle handle, const Error* error, AssetType type, std::string_view consequence)
		{
			const std::string_view typeName = AssetTypeToString(type);
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
					? std::format("asset {} cannot be used as a {}; {}", handle.ToString(), typeName, consequence)
					: std::format("asset {} is a {}, not a {}; {}", handle.ToString(), AssetTypeToString(actual), typeName, consequence);
				diagnostic.Hint = std::format("assign an asset of type {}", typeName);
			}
			else if (error->GetCode() == ErrorCode::NotFound)
			{
				diagnostic.Code = std::string(AssetMissingCode);
				diagnostic.Message = std::format("asset {} is missing; {}", handle.ToString(), consequence);
				diagnostic.Hint = "restore the asset, or assign another one";
			}
			else
			{
				diagnostic.Code = std::string(AssetImportFailedCode);
				diagnostic.Message = std::format("asset {} failed to load; {}: {}", handle.ToString(), consequence,
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
			// asset of another type, gives a null Sheet, reported once per plan (ReportUnavailableAsset).
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
					ReportUnavailableAsset(*m_Assets, handle, loaded.has_value() ? nullptr : &loaded.error(), AssetType::SpriteSheet, "the UI image draws nothing");
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

			// The chain of a text in the font `handle` (C22.3, decision 42): BuildUiFontChain's fonts, each resolved by
			// FindFont, the text's own font by `handle` itself (so null stays the Default font's placeholder, as before).
			[[nodiscard]] const PlannedChain& FindChain(AssetHandle handle)
			{
				if (const auto found = std::ranges::find(m_Chains, handle, &PlannedChain::Font); found != m_Chains.end())
					return *found;
				PlannedChain& planned = m_Chains.emplace_back();
				planned.Font = handle;
				const std::vector<AssetHandle> chain = BuildUiFontChain(handle, *m_Assets);
				for (size_t index = 0; index < chain.size(); ++index)
				{
					const AssetHandle runFont = index == 0 ? handle : chain[index];
					const FontData* font = FindFont(runFont);
					// Not even the placeholder is a font: the text draws nothing. (A fallback in the chain loaded as a font.)
					if (font == nullptr)
						break;
					planned.RunFonts.push_back(runFont);
					planned.Fonts.push_back(font);
				}
				return planned;
			}
		private:
			AssetManager* m_Assets = nullptr; // documented back-reference for the plan's duration
			std::vector<PlannedSheet> m_Sheets{};
			std::vector<PlannedFont> m_Fonts{};
			std::vector<PlannedChain> m_Chains{};
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
				case UiDrawAlignment::Start:          return RenderTextAlignment::Left;
				case UiDrawAlignment::Center:         return RenderTextAlignment::Center;
				case UiDrawAlignment::End:            return RenderTextAlignment::Right;
				case UiDrawAlignment::ParagraphStart: return RenderTextAlignment::Start;
				case UiDrawAlignment::ParagraphEnd:   return RenderTextAlignment::End;
			}
			return RenderTextAlignment::Left;
		}

		// The share of the height a text block leaves above it: Start none, Center half, End all.
		static float GetVerticalShare(UiDrawAlignment alignment)
		{
			switch (alignment)
			{
				case UiDrawAlignment::Start:          return 0.0f;
				case UiDrawAlignment::Center:         return 0.5f;
				case UiDrawAlignment::End:            return 1.0f;
				case UiDrawAlignment::ParagraphStart: return 0.0f; // horizontal only; never a vertical alignment
				case UiDrawAlignment::ParagraphEnd:   return 1.0f;
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
		// kind, clip, font, colour and (C22.3) World canvas, or to a new run. A quad run keeps a null font and a white colour
		// (the batcher takes each quad's own), and an unclipped run an empty clip.
		static void AppendToRun(UiPassPlan& plan, UiPassRunKind kind, bool clipped, const UiClipRect& clip, AssetHandle font, const glm::vec4& color,
			uint32_t first, uint32_t count, uint32_t worldCanvas = UiScreenCanvas)
		{
			const bool glyphs = kind == UiPassRunKind::Glyphs;
			const UiPassRun run{ .Kind = kind,
				.First = first,
				.Count = count,
				.Clipped = clipped,
				.Clip = clipped ? clip : UiClipRect{},
				.Font = glyphs ? font : AssetHandle{},
				.Color = glyphs ? color : glm::vec4(1.0f),
				.WorldCanvas = worldCanvas };
			if (!plan.Runs.empty())
			{
				UiPassRun& last = plan.Runs.back();
				if (last.Kind == run.Kind && last.Clipped == run.Clipped && last.Clip == run.Clip && last.Font == run.Font && last.Color == run.Color
					&& last.WorldCanvas == run.WorldCanvas && last.First + last.Count == first)
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

		// A text input's caret (C22.3, decision 48; PlanUiPass's caret rule) in a block whose top-left corner is `topLeft`:
		// a quad of `color` with the white texel, `width` scene pixels wide, its left edge at the caret's x and spanning its
		// line's LineHeight.
		static UiPassQuad MakeUiCaretQuad(const TextMeasurement& measured, const TextCaret& caret, glm::vec2 topLeft, float width, const glm::vec4& color)
		{
			const glm::vec2 min = topLeft + caret.Top;
			return UiPassQuad{ .Min = min, .Max = glm::vec2(min.x + width, min.y + measured.LineHeight), .Color = color };
		}

		// Measures `text` with the font chain `chain` (not empty) by `options` and appends its glyphs with the block's top-left
		// corner at `origin` plus the share `verticalShare` of the height `height` leaves, in glyph runs of `color`, one per
		// sequence of glyphs of one font of the chain (its RunFonts). With `caretByte` (a text item's HasCaret, C22.3), the
		// block is aligned as at least one LineHeight high, its caret before that byte follows the glyphs as a quad, and a block
		// that does not wrap moves by the least distance, left or right, that puts the caret's quad inside
		// [origin.x, origin.x + options.MaxWidth] (its left edge at origin.x when the quad is wider). Returns whether any glyph
		// or caret was appended.
		static bool PlanGlyphs(UiPassPlan& plan, const PlannedChain& chain, std::string_view text, const TextMeasureOptions& options, glm::vec2 origin, float height,
			float verticalShare, const glm::vec4& color, bool clipped, const UiClipRect& clip, std::optional<uint32_t> caretByte = std::nullopt)
		{
			const TextMeasurement measured = MeasureText(chain.Fonts, text, options);
			if (measured.Glyphs.empty() && !caretByte.has_value())
				return false;
			const float blockHeight = caretByte.has_value() ? std::max(measured.Size.y, measured.LineHeight) : measured.Size.y;
			glm::vec2 topLeft(origin.x, origin.y + (height - blockHeight) * verticalShare);
			std::optional<TextCaret> caret;
			float caretWidth = 0.0f;
			if (caretByte.has_value())
			{
				caret = FindTextCaret(measured, *caretByte);
				caretWidth = std::max(1.0f, std::floor(options.Size / 16.0f + 0.5f));
				if (options.Wrap == TextWrap::None)
				{
					const float left = topLeft.x + caret->Top.x;
					const float rectangleRight = origin.x + options.MaxWidth;
					if (caretWidth > options.MaxWidth || left < origin.x)
						topLeft.x += origin.x - left;
					else if (left + caretWidth > rectangleRight)
						topLeft.x += rectangleRight - (left + caretWidth);
				}
			}
			for (const TextMeasuredGlyph& glyph : measured.Glyphs)
			{
				plan.Glyphs.push_back(
					TextPixelGlyph{ .Min = glyph.Min + topLeft, .Max = glyph.Max + topLeft, .AtlasMin = glyph.AtlasMin, .AtlasMax = glyph.AtlasMax });
				// A glyph extends its predecessor's run while their font is the same; a fallback's glyph starts a run of its own.
				AppendToRun(plan, UiPassRunKind::Glyphs, clipped, clip, chain.RunFonts[glyph.Font], color, static_cast<uint32_t>(plan.Glyphs.size() - 1), 1);
			}
			if (caret.has_value())
				AppendQuad(plan, MakeUiCaretQuad(measured, *caret, topLeft, caretWidth, color), clipped, clip);
			return true;
		}

		// Plans a text item (not clipped away) with its font's chain; returns whether it drew a glyph or its caret (C22.3: an
		// empty text with a caret draws the caret alone).
		static bool PlanText(const UiDrawItem& item, UiPlanResolver& resolver, UiPassPlan& plan)
		{
			if ((item.Text.empty() && !item.HasCaret) || !IsFinite(item.Min) || !IsFinite(item.Max) || !IsPositive(item.FontSize) || !IsPositive(item.LineSpacing))
				return false;
			const PlannedChain& chain = resolver.FindChain(item.Font);
			if (chain.Fonts.empty())
				return false;
			const TextMeasureOptions options{ .Size = item.FontSize,
				.LineSpacing = item.LineSpacing,
				.Wrap = item.Wrap ? TextWrap::Word : TextWrap::None,
				.MaxWidth = item.Max.x - item.Min.x,
				.Alignment = ToRenderTextAlignment(item.HorizontalAlignment) };
			return PlanGlyphs(plan, chain, item.Text, options, item.Min, item.Max.y - item.Min.y, GetVerticalShare(item.VerticalAlignment), item.Color,
				item.Clipped, item.Clip, item.HasCaret ? std::optional<uint32_t>(item.CaretByte) : std::nullopt);
		}

		// C22.3 (decision 51): a World canvas item's clip on the CPU, in its canvas pixels: the canvas's rectangle, (0, 0) to its
		// Size (the canvas is drawn only inside it), cut by the item's clip when it is clipped (its pixels [MinX, MaxX) x
		// [MinY, MaxY) as the rectangle they cover).
		struct UiWorldClip
		{
			glm::vec2 Min = glm::vec2(0.0f);
			glm::vec2 Max = glm::vec2(0.0f);
		};

		static UiWorldClip MakeUiWorldClip(const UiDrawItem& item, const UiWorldCanvas& canvas)
		{
			UiWorldClip clip{ .Min = glm::vec2(0.0f), .Max = glm::vec2(static_cast<float>(canvas.Size.x), static_cast<float>(canvas.Size.y)) };
			if (item.Clipped)
			{
				clip.Min = glm::max(clip.Min, glm::vec2(static_cast<float>(item.Clip.MinX), static_cast<float>(item.Clip.MinY)));
				clip.Max = glm::min(clip.Max, glm::vec2(static_cast<float>(item.Clip.MaxX), static_cast<float>(item.Clip.MaxY)));
			}
			return clip;
		}

		// A rectangle [min, max] with its texture rectangle [texMin, texMax] cut to `clip`: each edge that moves takes the
		// texture coordinate at its new place (exact for an axis-aligned quad), each edge that stays keeps its own. False when
		// nothing of it is left.
		static bool CutUiWorldRect(const UiWorldClip& clip, glm::vec2& min, glm::vec2& max, glm::vec2& texMin, glm::vec2& texMax)
		{
			const glm::vec2 cutMin = glm::max(min, clip.Min);
			const glm::vec2 cutMax = glm::min(max, clip.Max);
			if (!(cutMax.x > cutMin.x && cutMax.y > cutMin.y))
				return false;
			const glm::vec2 size = max - min;
			const glm::vec2 texSize = texMax - texMin;
			glm::vec2 cutTexMin = texMin;
			glm::vec2 cutTexMax = texMax;
			for (glm::length_t axis = 0; axis < 2; ++axis)
			{
				if (cutMin[axis] != min[axis])
					cutTexMin[axis] = texMin[axis] + texSize[axis] * ((cutMin[axis] - min[axis]) / size[axis]);
				if (cutMax[axis] != max[axis])
					cutTexMax[axis] = texMin[axis] + texSize[axis] * ((cutMax[axis] - min[axis]) / size[axis]);
			}
			min = cutMin;
			max = cutMax;
			texMin = cutTexMin;
			texMax = cutTexMax;
			return true;
		}

		// Appends a World canvas item's quads and glyphs, planned in its canvas pixels into the item's own plan `planned`, to
		// `plan`, each cut to the item's clip on the CPU (MakeUiWorldClip), in runs of the canvas `worldCanvas` (an index of
		// plan.WorldCanvases) without a scissor. Returns whether anything was left to draw.
		static bool AppendUiWorldItem(UiPassPlan& plan, const UiPassPlan& planned, const UiDrawItem& item, uint32_t worldCanvas)
		{
			const UiWorldClip clip = MakeUiWorldClip(item, plan.WorldCanvases[worldCanvas]);
			bool drew = false;
			for (const UiPassRun& run : planned.Runs)
			{
				const size_t end = static_cast<size_t>(run.First) + run.Count;
				if (run.Kind == UiPassRunKind::Quads)
				{
					for (size_t index = run.First; index < std::min(end, planned.Quads.size()); ++index)
					{
						UiPassQuad quad = planned.Quads[index];
						if (!CutUiWorldRect(clip, quad.Min, quad.Max, quad.TexCoordMin, quad.TexCoordMax))
							continue;
						quad.WorldCanvas = worldCanvas;
						plan.Quads.push_back(quad);
						AppendToRun(plan, UiPassRunKind::Quads, false, UiClipRect{}, AssetHandle{}, glm::vec4(1.0f),
							static_cast<uint32_t>(plan.Quads.size() - 1), 1, worldCanvas);
						drew = true;
					}
					continue;
				}
				for (size_t index = run.First; index < std::min(end, planned.Glyphs.size()); ++index)
				{
					TextPixelGlyph glyph = planned.Glyphs[index];
					if (!CutUiWorldRect(clip, glyph.Min, glyph.Max, glyph.AtlasMin, glyph.AtlasMax))
						continue;
					plan.Glyphs.push_back(glyph);
					AppendToRun(plan, UiPassRunKind::Glyphs, false, UiClipRect{}, run.Font, run.Color, static_cast<uint32_t>(plan.Glyphs.size() - 1), 1,
						worldCanvas);
					drew = true;
				}
			}
			return drew;
		}

		// The order RecordUiPass records `plan`'s runs in (UiPass.h's file comment, C22.3): with a camera, the World canvases'
		// runs first, canvas by canvas from the farthest to the nearest (the view-space depth of each canvas's centre, the most
		// negative first; ties in the plan's order), each canvas's runs in plan order; then the Screen canvases' runs in plan
		// order. Without a camera no World canvas's run is recorded.
		static std::vector<uint32_t> OrderUiPassRuns(const UiPassPlan& plan, const UiPassInputs& inputs)
		{
			std::vector<uint32_t> order;
			order.reserve(plan.Runs.size());
			if (inputs.HasCamera && !plan.WorldCanvases.empty())
			{
				std::vector<float> depths;
				std::vector<uint32_t> canvases;
				depths.reserve(plan.WorldCanvases.size());
				canvases.reserve(plan.WorldCanvases.size());
				for (const UiWorldCanvas& canvas : plan.WorldCanvases)
				{
					const glm::vec4 centre(static_cast<float>(canvas.Size.x) * 0.5f, static_cast<float>(canvas.Size.y) * 0.5f, 0.0f, 1.0f);
					depths.push_back((inputs.View * (canvas.World * centre)).z);
					canvases.push_back(static_cast<uint32_t>(canvases.size()));
				}
				std::ranges::stable_sort(canvases, [&depths](uint32_t a, uint32_t b)
				{
					return depths[a] < depths[b];
				});
				for (const uint32_t canvas : canvases)
				{
					for (uint32_t run = 0; run < plan.Runs.size(); ++run)
					{
						if (plan.Runs[run].WorldCanvas == canvas)
							order.push_back(run);
					}
				}
			}
			for (uint32_t run = 0; run < plan.Runs.size(); ++run)
			{
				if (plan.Runs[run].WorldCanvas == UiScreenCanvas)
					order.push_back(run);
			}
			return order;
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
		plan.WorldCanvases.assign(list.WorldCanvases.begin(), list.WorldCanvases.end());
		// C22.3: a World canvas's item is planned on its own in its canvas pixels, then cut to its clip on the CPU.
		UiPassPlan worldItem;
		for (const UiDrawItem& item : list.Items)
		{
			// An item whose clipping ancestors leave it no pixel draws nothing.
			if (item.Clipped && item.Clip.IsEmpty())
				continue;
			if (item.WorldCanvas != UiScreenCanvas)
			{
				// An index the list does not hold names no canvas: the item draws nothing.
				if (item.WorldCanvas >= plan.WorldCanvases.size())
					continue;
				worldItem = UiPassPlan{};
				const bool planned = item.Kind == UiDrawKind::Image ? Utils::PlanImage(item, resolver, worldItem) : Utils::PlanText(item, resolver, worldItem);
				plan.SkippedItems += worldItem.SkippedItems;
				const bool drew = planned && Utils::AppendUiWorldItem(plan, worldItem, item, item.WorldCanvas);
				plan.DrawnItems += drew ? 1U : 0U;
				continue;
			}
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
			// Left-aligned in the Default font (and its chain), its block's bottom-left corner at the rectangle's top-left corner.
			const PlannedChain& chain = resolver.FindChain(AssetHandle{});
			if (chain.Fonts.empty())
				continue;
			const TextMeasureOptions options{ .Size = UiAnnotationLabelSize, .Alignment = RenderTextAlignment::Left };
			Utils::PlanGlyphs(plan, chain, annotation.Label, options, annotation.Min, 0.0f, 1.0f, annotation.Color, false, UiClipRect{});
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
		// C22.3: the World canvas a run is in, or null for a Screen canvas's run (and for an index the plan does not hold,
		// whose runs are not in the order below).
		const auto findWorldCanvas = [&plan](const UiPassRun& run) -> const UiWorldCanvas*
		{
			return run.WorldCanvas < plan.WorldCanvases.size() ? &plan.WorldCanvases[run.WorldCanvas] : nullptr;
		};
		const std::vector<uint32_t> order = Utils::OrderUiPassRuns(plan, inputs);
		std::vector<QuadDraw> quads;
		std::vector<TextGlyphRun> glyphRuns;
		size_t position = 0;
		while (position < order.size())
		{
			const UiPassRun& run = plan.Runs[order[position]];
			if (run.Kind == UiPassRunKind::Quads)
			{
				++position;
				const bool inside = static_cast<size_t>(run.First) + run.Count <= plan.Quads.size();
				ENGINE_CORE_ASSERT(inside, "a Ui run's quads [{}, +{}) lie outside the plan's {} quads", run.First, run.Count, plan.Quads.size());
				if (!inside)
					continue;
				// A World canvas's quads are placed in the world by its matrix (C22.3); a Screen canvas's stay in scene pixels.
				const UiWorldCanvas* world = findWorldCanvas(run);
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
					std::array<glm::vec3, 4> corners = { glm::vec3(quad.Min.x, quad.Min.y, 0.0f), glm::vec3(quad.Max.x, quad.Min.y, 0.0f),
						glm::vec3(quad.Max.x, quad.Max.y, 0.0f), glm::vec3(quad.Min.x, quad.Max.y, 0.0f) };
					if (world != nullptr)
					{
						for (glm::vec3& corner : corners)
							corner = glm::vec3(world->World * glm::vec4(corner, 1.0f));
					}
					quads.push_back(QuadDraw{ .Corners = corners,
						.TexCoordMin = quad.TexCoordMin,
						.TexCoordMax = quad.TexCoordMax,
						.Color = quad.Color,
						.Texture = texture,
						.Sampler = Utils::ToQuadSampler(quad.Filter) });
				}
				const QuadBatchInputs batch{ .Quads = quads,
					.Target = world != nullptr ? QuadBatchTarget::LdrDepth : QuadBatchTarget::Ldr,
					.Framebuffer = inputs.Framebuffer,
					.Scissor = run.Clipped ? std::optional<nvrhi::Rect>(Utils::ToScissor(run.Clip)) : std::nullopt,
					.Transform = world != nullptr ? inputs.ViewProjection : transform };
				const Result<uint32_t> recorded = inputs.Quads->Record(commandList, quadBindings, batch, counters);
				if (!recorded)
					keepError(std::unexpected(recorded.error()));
				continue;
			}

			// Consecutive glyph runs record together, each with its own scissor (a World canvas's with its matrix instead).
			glyphRuns.clear();
			for (; position < order.size() && plan.Runs[order[position]].Kind == UiPassRunKind::Glyphs; ++position)
			{
				const UiPassRun& glyphs = plan.Runs[order[position]];
				const bool inside = static_cast<size_t>(glyphs.First) + glyphs.Count <= plan.Glyphs.size();
				ENGINE_CORE_ASSERT(inside, "a Ui run's glyphs [{}, +{}) lie outside the plan's {} glyphs", glyphs.First, glyphs.Count, plan.Glyphs.size());
				if (!inside)
					continue;
				const UiWorldCanvas* world = findWorldCanvas(glyphs);
				glyphRuns.push_back(TextGlyphRun{ .Font = glyphs.Font,
					.Color = glyphs.Color,
					.FirstGlyph = glyphs.First,
					.GlyphCount = glyphs.Count,
					.Scissor = glyphs.Clipped && world == nullptr ? std::optional<nvrhi::Rect>(Utils::ToScissor(glyphs.Clip)) : std::nullopt,
					.World = world != nullptr ? std::optional<glm::mat4>(world->World) : std::nullopt });
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

	std::vector<AssetHandle> BuildUiFontChain(AssetHandle font, AssetManager& assets)
	{
		// The text's own font: null is the Default font, served as the font placeholder without a diagnostic; a missing or
		// failed font is its placeholder, reported once by GetOrPlaceholder (as PlanUiPass resolved a text's font before).
		const AssetRef<FontData> own = font.IsValid() ? assets.GetOrPlaceholder<FontData>(font) : AssetCast<FontData>(assets.GetPlaceholder(AssetType::Font));
		std::vector<AssetHandle> chain = { font.IsValid() ? font : BuiltinAssetHandles::DefaultFont };

		// Depth first: a font's fallbacks in their order, each fallback's own right after it. `pending` holds the fonts whose
		// fallbacks are being walked, innermost last, each with the index of its next fallback.
		std::vector<std::pair<AssetRef<FontData>, size_t>> pending;
		if (own != nullptr)
			pending.emplace_back(own, 0);
		while (!pending.empty() && chain.size() < MaxUiFontChainLength)
		{
			auto& [current, next] = pending.back();
			if (next >= current->Fallbacks.size())
			{
				pending.pop_back();
				continue;
			}
			const AssetHandle fallback = current->Fallbacks[next++];
			// Each font once (a fallback that names an earlier font of the chain adds nothing, so cycles end); a null handle
			// names no font (a loaded font holds none, ValidateFontData).
			if (!fallback.IsValid() || std::ranges::find(chain, fallback) != chain.end())
				continue;
			const Result<AssetRef<Asset>> loaded = assets.Load(fallback);
			AssetRef<FontData> fallbackFont = loaded.has_value() ? AssetCast<FontData>(*loaded) : AssetRef<FontData>{};
			if (fallbackFont == nullptr)
			{
				ReportUnavailableAsset(assets, fallback, loaded.has_value() ? nullptr : &loaded.error(), AssetType::Font,
					"UI text leaves it out of its font chain");
				continue;
			}
			chain.push_back(fallback);
			pending.emplace_back(std::move(fallbackFont), 0);
		}
		return chain;
	}

}
