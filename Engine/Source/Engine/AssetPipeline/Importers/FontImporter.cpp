#include "EnginePCH.h"
#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/AssetPipeline/Private/TrueTypeFont.h"
#include "Engine/AssetPipeline/Private/TrueTypeTables.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <utility>

namespace Engine {

	namespace {

		// One glyph of the atlas being built: its codepoint, glyph index, metrics, distance field and atlas position.
		struct BakedGlyph
		{
			uint32_t Codepoint = 0;
			int32_t Glyph = 0;
			int32_t Advance = 0; // font units
			TrueTypeGlyphSdf Sdf{};
			uint32_t AtlasX = 0;
			uint32_t AtlasY = 0;
		};

		// An inclusive codepoint range of the imported character set.
		struct CodepointRange
		{
			uint32_t First = 0;
			uint32_t Last = 0;
		};

	}

	namespace Utils {

		// ASCII printable characters and Latin-1's printable half (§7.4: "ASCII + Latin-1").
		static constexpr std::array<CodepointRange, 2> FontCharacterSet = { { { 0x20, 0x7E }, { 0xA0, 0xFF } } };
		// Free pixels between two glyphs of the atlas, so bilinear sampling at a glyph's edge never reads its neighbour.
		static constexpr uint32_t AtlasGlyphGap = 1;
		// The distance-field value of an outline (stb_truetype's on-edge value): larger inside, smaller outside.
		static constexpr float SdfEdgeValue = 128.0f;
		// The smallest atlas side tried.
		static constexpr uint32_t MinAtlasSize = 16;
		static constexpr uint32_t MaxAtlasSize = 16384;

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

		// Places the glyphs with a distance field on shelves, in codepoint order, into a `size` x `size` atlas: left to
		// right, a new shelf below the tallest glyph of the previous one when the next glyph does not fit. Returns false
		// when they do not all fit.
		[[nodiscard]] static bool PackGlyphs(std::vector<BakedGlyph>& glyphs, uint32_t size)
		{
			uint32_t x = 0;
			uint32_t y = 0;
			uint32_t shelfHeight = 0;
			for (BakedGlyph& glyph : glyphs)
			{
				const uint32_t width = glyph.Sdf.Width;
				const uint32_t height = glyph.Sdf.Height;
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

		// The smallest power-of-two atlas side the glyphs fit in, with their positions set. Errors: ImportFailed when they
		// need more than MaxAtlasSize.
		[[nodiscard]] static Result<uint32_t> PackAtlas(std::vector<BakedGlyph>& glyphs, std::string_view name)
		{
			for (uint32_t size = MinAtlasSize; size <= MaxAtlasSize; size *= 2)
			{
				if (PackGlyphs(glyphs, size))
					return size;
			}
			return std::unexpected(Error(ErrorCode::ImportFailed, std::format("the glyphs of '{}' do not fit in a {}x{} atlas", name, MaxAtlasSize, MaxAtlasSize)).WithHint("lower PixelSize or Spread"));
		}

		// Rasterizes the character set of `source` into an SDF atlas with metrics and kerning (FontImporter.h).
		[[nodiscard]] static Result<FontData> BakeFont(std::span<const std::byte> source, const FontImportSettings& settings, std::string_view name)
		{
			ENGINE_TRY_ASSIGN(const TrueTypeFont font, WithContext(TrueTypeFont::Open(source), std::format("while reading the font '{}'", name)));
			const float unitsPerEm = static_cast<float>(font.GetUnitsPerEm());

			std::vector<BakedGlyph> glyphs;
			std::vector<TrueTypeGlyphEntry> entries;
			for (const CodepointRange& range : FontCharacterSet)
			{
				for (uint32_t codepoint = range.First; codepoint <= range.Last; ++codepoint)
				{
					const std::optional<int32_t> glyph = font.FindGlyph(codepoint);
					if (!glyph.has_value())
						continue;
					BakedGlyph baked;
					baked.Codepoint = codepoint;
					baked.Glyph = *glyph;
					baked.Advance = font.GetGlyphMetrics(*glyph).Advance;
					glyphs.push_back(std::move(baked));
					entries.push_back({ .Codepoint = codepoint, .Glyph = static_cast<uint16_t>(*glyph) });
				}
			}
			if (glyphs.empty())
				return MakeError(ErrorCode::ImportFailed, "the font '{}' has no glyph for any character of ASCII or Latin-1", name);

			const TrueTypeVerticalMetrics vertical = font.GetVerticalMetrics();
			if (vertical.Ascent < 0 || vertical.Descent > 0)
			{
				return MakeError(ErrorCode::ImportFailed, "the font '{}' has inverted vertical metrics (ascent {}, descent {})", name, vertical.Ascent,
					vertical.Descent);
			}

			// The field reaches 0 and 255 at Spread pixels outside and inside the outline; the padding holds that range.
			const int32_t padding = static_cast<int32_t>(std::ceil(settings.Spread));
			const float valuePerPixel = SdfEdgeValue / settings.Spread;
			for (BakedGlyph& glyph : glyphs)
			{
				ENGINE_TRY_ASSIGN(glyph.Sdf, WithContext(font.RenderGlyphSdf(glyph.Glyph, settings.PixelSize, padding, valuePerPixel), std::format("while rasterizing U+{:04X} of '{}'", glyph.Codepoint, name)));
			}
			ENGINE_TRY_ASSIGN(const uint32_t atlasSize, PackAtlas(glyphs, name));

			FontData data;
			data.PixelSize = settings.PixelSize;
			data.Spread = settings.Spread;
			data.Ascent = static_cast<float>(vertical.Ascent) / unitsPerEm;
			data.Descent = static_cast<float>(vertical.Descent) / unitsPerEm;
			data.LineGap = static_cast<float>(vertical.LineGap) / unitsPerEm;
			data.AtlasWidth = atlasSize;
			data.AtlasHeight = atlasSize;
			data.AtlasPixels.assign(static_cast<size_t>(atlasSize) * atlasSize, std::byte{ 0 });

			const float size = static_cast<float>(atlasSize);
			const float pixelsPerEm = settings.PixelSize;
			data.Glyphs.reserve(glyphs.size());
			for (const BakedGlyph& glyph : glyphs)
			{
				FontGlyph out;
				out.Codepoint = glyph.Codepoint;
				out.Advance = static_cast<float>(glyph.Advance) / unitsPerEm;
				const TrueTypeGlyphSdf& sdf = glyph.Sdf;
				if (sdf.Width != 0)
				{
					for (uint32_t row = 0; row < sdf.Height; ++row)
					{
						const auto rowStart = sdf.Pixels.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(row) * sdf.Width);
						const size_t target = (static_cast<size_t>(glyph.AtlasY) + row) * atlasSize + glyph.AtlasX;
						std::copy(rowStart, rowStart + sdf.Width, data.AtlasPixels.begin() + static_cast<std::ptrdiff_t>(target));
					}
					// The bitmap's top-left corner lies at (OffsetX, OffsetY) pixels from the pen, y down; plane coordinates
					// are em units, y up.
					out.PlaneMin = { static_cast<float>(sdf.OffsetX) / pixelsPerEm, -static_cast<float>(sdf.OffsetY + static_cast<int32_t>(sdf.Height)) / pixelsPerEm };
					out.PlaneMax = { static_cast<float>(sdf.OffsetX + static_cast<int32_t>(sdf.Width)) / pixelsPerEm, -static_cast<float>(sdf.OffsetY) / pixelsPerEm };
					out.AtlasMin = { static_cast<float>(glyph.AtlasX) / size, static_cast<float>(glyph.AtlasY) / size };
					out.AtlasMax = { static_cast<float>(glyph.AtlasX + sdf.Width) / size, static_cast<float>(glyph.AtlasY + sdf.Height) / size };
				}
				data.Glyphs.push_back(out);
			}

			ENGINE_TRY_ASSIGN(const std::vector<TrueTypeKerningEntry> kerning, WithContext(Utils::ReadTrueTypeKerning(source, entries), std::format("while reading the kerning of '{}'", name)));
			data.Kerning.reserve(kerning.size());
			for (const TrueTypeKerningEntry& pair : kerning)
				data.Kerning.push_back({ .First = pair.First, .Second = pair.Second, .Advance = static_cast<float>(pair.Adjustment) / unitsPerEm });

			ENGINE_TRY(WithContext(ValidateFontData(data), std::format("while baking the font '{}'", name)));
			return data;
		}

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
		ENGINE_TRY_ASSIGN(const FontData font, Utils::BakeFont(context.GetSourceBytes(), settings, sourcePath));

		ImportResult result;
		result.Artifacts.push_back(ImportedArtifact{ .Handle = metadata.Handle, .Type = AssetType::Font, .SubAssetKey = {}, .Cooked = CookFont(font, Version) });
		return result;
	}

	void FontImporter::RegisterTypes(TypeRegistry& registry)
	{
		registry.Struct<FontImportSettings>("FontImportSettings", "How a TTF or OTF font is baked into a signed-distance-field atlas.")
			.Field("PixelSize", &FontImportSettings::PixelSize, "Atlas pixels per em the glyphs are rasterized at.",
				{ .Min = 16.0, .Max = 128.0, .Unit = "px" })
			.Field("Spread", &FontImportSettings::Spread, "The distance field's range in atlas pixels on each side of an outline.",
				{ .Min = 1.0, .Max = 32.0, .Unit = "px" });
	}

}
