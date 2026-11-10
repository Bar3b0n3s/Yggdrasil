#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/AssetPipeline/Private/FontShaping.h"
#include "Engine/AssetPipeline/Private/TrueTypeFont.h"
#include "Engine/AssetPipeline/Private/TrueTypeTables.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// One glyph of the atlas being built: its codepoint (0 for a glyph only shaping reaches), glyph index, metrics,
		// distance-field box, atlas position and, once the boxes are packed, its distance field.
		struct BakedGlyph
		{
			uint32_t Codepoint = 0;
			int32_t Glyph = 0;
			int32_t Advance = 0; // font units
			TrueTypeGlyphBox Box{};
			uint32_t AtlasX = 0;
			uint32_t AtlasY = 0;
			TrueTypeGlyphSdf Sdf{};
		};

		// An inclusive codepoint range of the imported character set.
		struct CodepointRange
		{
			uint32_t First = 0;
			uint32_t Last = 0;
		};

	}

	namespace Utils {

		// ASCII printable characters and Latin-1's printable half (§7.4: "ASCII + Latin-1"): every font's character set, and
		// the glyphs the importer kerns (FontImporter.h).
		static constexpr std::array<CodepointRange, 2> FontCharacterSet = { { { 0x20, 0x7E }, { 0xA0, 0xFF } } };
		// The largest range the FontImportSettings Generate hook keeps, so its MaxFontCharacterRanges ranges stay within
		// MaxFontRangeCodepoints.
		static constexpr uint32_t MaxGeneratedRangeCodepoints = static_cast<uint32_t>(MaxFontRangeCodepoints / MaxFontCharacterRanges);
		// Free pixels between two glyphs of the atlas, so bilinear sampling at a glyph's edge never reads its neighbour.
		static constexpr uint32_t AtlasGlyphGap = 1;
		// The distance-field value of an outline (stb_truetype's on-edge value): larger inside, smaller outside.
		static constexpr float SdfEdgeValue = 128.0f;
		// The smallest atlas side tried.
		static constexpr uint32_t MinAtlasSize = 16;

		// The codepoints `ranges` names, overlaps counted in each range (FontImporter.h's limit).
		[[nodiscard]] static uint64_t CountRangeCodepoints(std::span<const FontCharacterRange> ranges)
		{
			uint64_t count = 0;
			for (const FontCharacterRange& range : ranges)
				count += range.Last >= range.First ? static_cast<uint64_t>(range.Last - range.First) + 1 : 0;
			return count;
		}

		// The "FontCharacterRange" Validate hook: First at most Last (the field ranges bound each).
		static void ValidateFontCharacterRange(const FontCharacterRange& range, ValidationContext& context)
		{
			if (range.First > range.Last)
				context.Error("Last", std::format("must be at least First (U+{:04X}), got U+{:04X}", range.First, range.Last));
		}

		// The "FontCharacterRange" Generate hook: a reversed range is turned around.
		static void GenerateFontCharacterRange(FontCharacterRange& range, Random& /*random*/)
		{
			if (range.First > range.Last)
				std::swap(range.First, range.Last);
		}

		// The "FontImportSettings" Validate hook: the limits on Ranges that bound the baker's work, and the rules on Fallbacks
		// that need no handle of the font's own (at most MaxFontFallbacks, none null; Import refuses the font itself).
		static void ValidateFontImportSettings(const FontImportSettings& settings, ValidationContext& context)
		{
			if (settings.Ranges.size() > MaxFontCharacterRanges)
				context.Error("Ranges", std::format("must hold at most {} ranges (got {})", MaxFontCharacterRanges, settings.Ranges.size()));
			const uint64_t codepoints = CountRangeCodepoints(settings.Ranges);
			if (codepoints > MaxFontRangeCodepoints)
				context.Error("Ranges", std::format("must name at most {} codepoints in all (got {})", MaxFontRangeCodepoints, codepoints));

			if (settings.Fallbacks.size() > MaxFontFallbacks)
				context.Error("Fallbacks", std::format("must name at most {} fonts (got {})", MaxFontFallbacks, settings.Fallbacks.size()));
			context.PushKey("Fallbacks");
			for (size_t index = 0; index < settings.Fallbacks.size(); ++index)
			{
				if (!settings.Fallbacks[index].IsValid())
					context.Error(std::to_string(index), "must name a font, not null");
			}
			context.PopKey();
		}

		// The "FontImportSettings" Generate hook: at most MaxFontCharacterRanges ranges (each already turned around by its own
		// hook), each cut to MaxGeneratedRangeCodepoints codepoints; Fallbacks without its null handles, cut to
		// MaxFontFallbacks.
		static void GenerateFontImportSettings(FontImportSettings& settings, Random& /*random*/)
		{
			if (settings.Ranges.size() > MaxFontCharacterRanges)
				settings.Ranges.resize(MaxFontCharacterRanges);
			for (FontCharacterRange& range : settings.Ranges)
				range.Last = std::min(range.Last, range.First + (MaxGeneratedRangeCodepoints - 1));
			std::erase_if(settings.Fallbacks, [](const TypedAssetHandle<AssetType::Font>& fallback)
			{
				return !fallback.IsValid();
			});
			if (settings.Fallbacks.size() > MaxFontFallbacks)
				settings.Fallbacks.resize(MaxFontFallbacks);
		}

		// The rule on Fallbacks that the Validate hook cannot check, since only the import knows the font's handle `font`:
		// no fallback is the font itself, each such element a Validation error located at /Fallbacks/<index>.
		[[nodiscard]] static Status ValidateFallbacksOfFont(const FontImportSettings& settings, AssetHandle font)
		{
			ValidationContext context;
			context.PushKey("Fallbacks");
			for (size_t index = 0; index < settings.Fallbacks.size(); ++index)
			{
				if (settings.Fallbacks[index].GetHandle() == font)
					context.Error(std::to_string(index), std::format("must not name the font itself ({})", font));
			}
			context.PopKey();
			return context.ToStatus("FontImportSettings");
		}

		// The cooked chain of a font imported with `settings` (FontData::Fallbacks): its Fallbacks' handles, in order.
		[[nodiscard]] static std::vector<AssetHandle> GetFallbackHandles(const FontImportSettings& settings)
		{
			std::vector<AssetHandle> handles;
			handles.reserve(settings.Fallbacks.size());
			for (const TypedAssetHandle<AssetType::Font>& fallback : settings.Fallbacks)
				handles.push_back(fallback.GetHandle());
			return handles;
		}

		// The codepoints a font with `settings` bakes: FontCharacterSet and the settings' Ranges, sorted and unique.
		[[nodiscard]] static std::vector<uint32_t> GetCharacterSet(const FontImportSettings& settings)
		{
			std::vector<uint32_t> codepoints;
			const auto append = [&codepoints](uint32_t first, uint32_t last)
			{
				for (uint64_t codepoint = first; codepoint <= last; ++codepoint)
					codepoints.push_back(static_cast<uint32_t>(codepoint));
			};
			for (const CodepointRange& range : FontCharacterSet)
				append(range.First, range.Last);
			for (const FontCharacterRange& range : settings.Ranges)
				append(range.First, range.Last);
			std::ranges::sort(codepoints);
			const auto [duplicates, end] = std::ranges::unique(codepoints);
			codepoints.erase(duplicates, end);
			return codepoints;
		}

		// True when `codepoint` is one of FontCharacterSet's, the glyphs the importer kerns.
		[[nodiscard]] static bool IsInBaseCharacterSet(uint32_t codepoint)
		{
			return std::ranges::any_of(FontCharacterSet, [codepoint](const CodepointRange& range)
			{
				return codepoint >= range.First && codepoint <= range.Last;
			});
		}

		[[nodiscard]] static Result<FontImportSettings> ReadSettings(const Json& settings, const TypeRegistry& registry)
		{
			FontImportSettings result;
			if (settings.is_null())
				return result;
			const StructInfo* type = registry.FindStruct<FontImportSettings>();
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no FontImportSettings (RegisterAssetPipelineTypes was not called)");
			ReadContext context;
			context.Strict = true;
			ENGINE_TRY(type->FromJson(&result, JsonReader(settings), context));
			return result;
		}

		// Places the boxes of the glyphs with a distance field on shelves, in codepoint order, into a `size` x `size` atlas:
		// left to right, a new shelf below the tallest box of the previous one when the next box does not fit. Returns false
		// when they do not all fit.
		[[nodiscard]] static bool PackGlyphs(std::vector<BakedGlyph>& glyphs, uint32_t size)
		{
			uint32_t x = 0;
			uint32_t y = 0;
			uint32_t shelfHeight = 0;
			for (BakedGlyph& glyph : glyphs)
			{
				const uint32_t width = glyph.Box.Width;
				const uint32_t height = glyph.Box.Height;
				if (width == 0)
					continue;
				if (width > size || height > size)
					return false;
				if (x + width > size)
				{
					y += shelfHeight + AtlasGlyphGap;
					x = 0;
					shelfHeight = 0;
				}
				if (y + height > size)
					return false;
				glyph.AtlasX = x;
				glyph.AtlasY = y;
				x += width + AtlasGlyphGap;
				shelfHeight = std::max(shelfHeight, height);
			}
			return true;
		}

		// The FontGlyph of `glyph`, packed in an atlas `atlasSize` pixels on a side, with its field copied into `atlas`.
		[[nodiscard]] static FontGlyph MakeFontGlyph(const BakedGlyph& glyph, float unitsPerEm, float pixelsPerEm, uint32_t atlasSize, Buffer& atlas)
		{
			FontGlyph out;
			out.Codepoint = glyph.Codepoint;
			out.GlyphIndex = static_cast<uint32_t>(glyph.Glyph);
			out.Advance = static_cast<float>(glyph.Advance) / unitsPerEm;
			// The field is its box, packed at (AtlasX, AtlasY) (TrueTypeFont::RenderGlyphSdf).
			const TrueTypeGlyphSdf& sdf = glyph.Sdf;
			if (sdf.Width != 0)
			{
				for (uint32_t row = 0; row < sdf.Height; ++row)
				{
					const auto rowStart = sdf.Pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row) * sdf.Width);
					const size_t target = (static_cast<size_t>(glyph.AtlasY) + row) * atlasSize + glyph.AtlasX;
					std::copy(rowStart, rowStart + sdf.Width, atlas.begin() + static_cast<std::ptrdiff_t>(target));
				}
				// The bitmap's top-left corner lies at (OffsetX, OffsetY) pixels from the pen, y down; plane coordinates are
				// em units, y up.
				const float size = static_cast<float>(atlasSize);
				out.PlaneMin = { static_cast<float>(sdf.OffsetX) / pixelsPerEm, -static_cast<float>(sdf.OffsetY + static_cast<int32_t>(sdf.Height)) / pixelsPerEm };
				out.PlaneMax = { static_cast<float>(sdf.OffsetX + static_cast<int32_t>(sdf.Width)) / pixelsPerEm, -static_cast<float>(sdf.OffsetY) / pixelsPerEm };
				out.AtlasMin = { static_cast<float>(glyph.AtlasX) / size, static_cast<float>(glyph.AtlasY) / size };
				out.AtlasMax = { static_cast<float>(glyph.AtlasX + sdf.Width) / size, static_cast<float>(glyph.AtlasY + sdf.Height) / size };
			}
			return out;
		}

		// The smallest power-of-two atlas side the glyphs' boxes fit in, with their positions set. Errors: ImportFailed when
		// they need more than MaxFontAtlasSize.
		[[nodiscard]] static Result<uint32_t> PackAtlas(std::vector<BakedGlyph>& glyphs, std::string_view name)
		{
			for (uint32_t size = MinAtlasSize; size <= MaxFontAtlasSize; size *= 2)
			{
				if (PackGlyphs(glyphs, size))
					return size;
			}
			return std::unexpected(Error(ErrorCode::ImportFailed, std::format("the glyphs of '{}' do not fit in a {}x{} atlas", name, MaxFontAtlasSize, MaxFontAtlasSize))
					.WithHint("lower PixelSize or Spread, or name fewer codepoints in Ranges"));
		}

	}

	Result<FontData> FontImporter::Bake(std::span<const std::byte> source, const FontImportSettings& settings, std::string_view name, FontBakeReport& report)
	{
		report = FontBakeReport{};
		ENGINE_TRY_ASSIGN(const TrueTypeFont font, WithContext(TrueTypeFont::Open(source), std::format("while reading the font '{}'", name)));
		const float unitsPerEm = static_cast<float>(font.GetUnitsPerEm());

		// Glyphs in codepoint order; only those of ASCII and Latin-1 are kerned (FontImporter.h).
		std::vector<BakedGlyph> glyphs;
		std::vector<TrueTypeGlyphEntry> entries;
		for (const uint32_t codepoint : Utils::GetCharacterSet(settings))
		{
			const std::optional<int32_t> glyph = font.FindGlyph(codepoint);
			if (!glyph.has_value())
				continue;
			BakedGlyph baked;
			baked.Codepoint = codepoint;
			baked.Glyph = *glyph;
			baked.Advance = font.GetGlyphMetrics(*glyph).Advance;
			glyphs.push_back(std::move(baked));
			if (Utils::IsInBaseCharacterSet(codepoint))
				entries.push_back({ .Codepoint = codepoint, .Glyph = static_cast<uint16_t>(*glyph) });
		}
		report.Glyphs = static_cast<uint32_t>(glyphs.size());
		if (glyphs.empty())
			return MakeError(ErrorCode::ImportFailed, "the font '{}' has no glyph for any character of ASCII, Latin-1 or its Ranges", name);

		// Version 5: the glyphs shaping reaches from the baked codepoints that none of them maps to, after them by glyph
		// index (FontImporter.h).
		const size_t codepointGlyphCount = glyphs.size();
		std::vector<uint32_t> bakedCodepoints;
		std::vector<uint32_t> bakedGlyphs;
		bakedCodepoints.reserve(codepointGlyphCount);
		for (const BakedGlyph& glyph : glyphs)
		{
			bakedCodepoints.push_back(glyph.Codepoint);
			bakedGlyphs.push_back(static_cast<uint32_t>(glyph.Glyph));
		}
		std::ranges::sort(bakedGlyphs);
		const auto [repeated, uniqueEnd] = std::ranges::unique(bakedGlyphs);
		bakedGlyphs.erase(repeated, uniqueEnd);
		ENGINE_TRY_ASSIGN(const FontShapingClosure closure, WithContext(Utils::FindFontShapingClosure(source, bakedCodepoints), std::format("while finding the glyphs shaping reaches in '{}'", name)));
		for (const uint32_t glyph : closure.Glyphs)
		{
			if (std::ranges::binary_search(bakedGlyphs, glyph))
				continue;
			BakedGlyph shaped;
			shaped.Glyph = static_cast<int32_t>(glyph);
			shaped.Advance = font.GetGlyphMetrics(shaped.Glyph).Advance;
			glyphs.push_back(std::move(shaped));
			++report.ShapingGlyphs;
		}
		report.Glyphs = static_cast<uint32_t>(glyphs.size());

		const TrueTypeVerticalMetrics vertical = font.GetVerticalMetrics();
		if (vertical.Ascent < 0 || vertical.Descent > 0)
		{
			return MakeError(ErrorCode::ImportFailed, "the font '{}' has inverted vertical metrics (ascent {}, descent {})", name, vertical.Ascent,
				vertical.Descent);
		}

		// Measure and pack first: a field costs memory and solver work for each of its pixels, so a character set that
		// cannot fit the atlas fails before any field is rasterized. The field reaches 0 and 255 at Spread pixels outside and
		// inside the outline; the padding holds that range.
		const int32_t padding = static_cast<int32_t>(std::ceil(settings.Spread));
		for (BakedGlyph& glyph : glyphs)
		{
			ENGINE_TRY_ASSIGN(glyph.Box, WithContext(font.GetGlyphSdfBox(glyph.Glyph, settings.PixelSize, padding), std::format("while measuring U+{:04X} (glyph {}) of '{}'", glyph.Codepoint, glyph.Glyph, name)));
		}
		ENGINE_TRY_ASSIGN(const uint32_t atlasSize, Utils::PackAtlas(glyphs, name));
		report.AtlasSize = atlasSize;

		const float valuePerPixel = Utils::SdfEdgeValue / settings.Spread;
		for (BakedGlyph& glyph : glyphs)
		{
			if (glyph.Box.Width == 0)
				continue;
			ENGINE_TRY_ASSIGN(glyph.Sdf, WithContext(font.RenderGlyphSdf(glyph.Glyph, glyph.Box, settings.PixelSize, valuePerPixel), std::format("while rasterizing U+{:04X} (glyph {}) of '{}'", glyph.Codepoint, glyph.Glyph, name)));
			++report.RasterizedGlyphs;
		}

		FontData data;
		data.PixelSize = settings.PixelSize;
		data.Spread = settings.Spread;
		data.Ascent = static_cast<float>(vertical.Ascent) / unitsPerEm;
		data.Descent = static_cast<float>(vertical.Descent) / unitsPerEm;
		data.LineGap = static_cast<float>(vertical.LineGap) / unitsPerEm;
		data.AtlasWidth = atlasSize;
		data.AtlasHeight = atlasSize;
		data.AtlasPixels.assign(static_cast<size_t>(atlasSize) * atlasSize, std::byte{ 0 });

		// The codepoint glyphs in codepoint order; every baked glyph once by glyph index, a codepoint glyph with its smallest
		// codepoint (the first in codepoint order) and a glyph only shaping reaches with Codepoint 0.
		data.Glyphs.reserve(codepointGlyphCount);
		data.IndexedGlyphs.reserve(glyphs.size());
		for (size_t index = 0; index < glyphs.size(); ++index)
		{
			const FontGlyph out = Utils::MakeFontGlyph(glyphs[index], unitsPerEm, settings.PixelSize, atlasSize, data.AtlasPixels);
			if (index < codepointGlyphCount)
				data.Glyphs.push_back(out);
			data.IndexedGlyphs.push_back(out);
		}
		std::ranges::stable_sort(data.IndexedGlyphs, {}, &FontGlyph::GlyphIndex);
		const auto [laterCodepoints, indexedEnd] = std::ranges::unique(data.IndexedGlyphs, {}, &FontGlyph::GlyphIndex);
		data.IndexedGlyphs.erase(laterCodepoints, indexedEnd);
		data.UnitsPerEm = static_cast<uint32_t>(font.GetUnitsPerEm());

		ENGINE_TRY_ASSIGN(const std::vector<TrueTypeKerningEntry> kerning, WithContext(Utils::ReadTrueTypeKerning(source, entries), std::format("while reading the kerning of '{}'", name)));
		data.Kerning.reserve(kerning.size());
		for (const TrueTypeKerningEntry& pair : kerning)
			data.Kerning.push_back({ .First = pair.First, .Second = pair.Second, .Advance = static_cast<float>(pair.Adjustment) / unitsPerEm });

		// The subset shaping reads: the baked codepoints (and the mirrored forms right-to-left runs draw) and every baked
		// glyph, by its index.
		std::vector<uint32_t> subsetGlyphs;
		subsetGlyphs.reserve(data.IndexedGlyphs.size());
		for (const FontGlyph& glyph : data.IndexedGlyphs)
			subsetGlyphs.push_back(glyph.GlyphIndex);
		ENGINE_TRY_ASSIGN(data.ShapingSource, WithContext(Utils::SubsetFontForShaping(source, closure.Codepoints, subsetGlyphs), std::format("while subsetting '{}' for shaping", name)));

		ENGINE_TRY(WithContext(ValidateFontData(data), std::format("while baking the font '{}'", name)));
		return data;
	}

	std::span<const std::string_view> FontImporter::GetExtensions() const
	{
		static constexpr std::array<std::string_view, 2> Extensions = { ".ttf", ".otf" };
		return Extensions;
	}

	Result<ImportResult> FontImporter::Import(ImportContext& context, const AssetMetadata& metadata) const
	{
		const std::string sourcePath = context.GetSourcePath().ToString();
		ENGINE_TRY_ASSIGN(const FontImportSettings settings, WithContext(Utils::ReadSettings(context.GetSettings(), context.GetRegistry()), std::format("while reading the import settings of '{}'", sourcePath)));
		ENGINE_TRY(WithContext(Utils::ValidateFallbacksOfFont(settings, metadata.Handle), std::format("while reading the import settings of '{}'", sourcePath)));
		FontBakeReport report;
		ENGINE_TRY_ASSIGN(FontData font, Bake(context.GetSourceBytes(), settings, sourcePath, report));
		// The chain's fallbacks (C22.3, Docs/Decisions/0033-m22-contract.md decision 42), cooked in order and recorded as the
		// import's dependencies, sorted and unique, so an export that prunes keeps them; a fallback need not exist yet.
		font.Fallbacks = Utils::GetFallbackHandles(settings);

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Font, .SubAssetKey = {}, .Cooked = CookFont(font, Version) });
		result.Dependencies = font.Fallbacks;
		std::ranges::sort(result.Dependencies);
		const auto [duplicates, end] = std::ranges::unique(result.Dependencies);
		result.Dependencies.erase(duplicates, end);
		return result;
	}

	Status FontImporter::ValidateSource(std::span<const std::byte> source)
	{
		return Utils::ValidateTrueTypeTables(source);
	}

	void FontImporter::RegisterTypes(TypeRegistry& registry)
	{
		// The rules across fields and elements (First at most Last; the number of ranges and of their codepoints) are Validate
		// hooks, so a bad range is located at its own element and an exceeded limit at /Ranges.
		registry.Struct<FontCharacterRange>("FontCharacterRange", "An inclusive range of Unicode codepoints a font bakes besides ASCII and Latin-1.")
			.Field("First", &FontCharacterRange::First, "The range's first codepoint.",
				{ .Min = static_cast<double>(MinFontRangeCodepoint), .Max = static_cast<double>(MaxFontRangeCodepoint) })
			.Field("Last", &FontCharacterRange::Last, "The range's last codepoint, at least First.",
				{ .Min = static_cast<double>(MinFontRangeCodepoint), .Max = static_cast<double>(MaxFontRangeCodepoint) })
			.Validate(&Utils::ValidateFontCharacterRange)
			.Generate(&Utils::GenerateFontCharacterRange);

		registry.Struct<FontImportSettings>("FontImportSettings", "How a TTF or OTF font is baked into a signed-distance-field atlas.")
			.Field("PixelSize", &FontImportSettings::PixelSize, "Atlas pixels per em the glyphs are rasterized at.",
				{ .Min = 16.0, .Max = 128.0, .Unit = "px" })
			.Field("Spread", &FontImportSettings::Spread, "The distance field's range in atlas pixels on each side of an outline.",
				{ .Min = 1.0, .Max = 32.0, .Unit = "px" })
			.Field("Ranges", &FontImportSettings::Ranges, "Codepoint ranges baked besides ASCII and Latin-1; codepoints the font lacks are left out.")
			.Field("Fallbacks", &FontImportSettings::Fallbacks,
				"Fonts a text in this font falls back to, in order, for a codepoint it lacks; at most 8, recorded as the import's dependencies.")
			.Validate(&Utils::ValidateFontImportSettings)
			.Generate(&Utils::GenerateFontImportSettings);
	}

}
