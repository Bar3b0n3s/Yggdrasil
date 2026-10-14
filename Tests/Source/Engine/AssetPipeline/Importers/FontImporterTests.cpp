#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/Fuzz.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>

namespace Engine {

	namespace {

		// The committed Inter-Regular.ttf (Resources/Fonts, SIL OFL).
		Buffer ReadInter()
		{
			Result<Buffer> font = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf");
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			return std::move(*font);
		}

		// A committed font fixture of Tests/Data/Assets/Fonts: Glyphs.ttf (MakeFormatFixtures.py) or Inter-Regular.otf (pinned
		// by FetchAssets.py fixture).
		Buffer ReadFontFixture(std::string_view name)
		{
			Result<Buffer> font = FileSystem::ReadFile(Test::GetTestDataPath("Assets/Fonts") / std::string(name));
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			return std::move(*font);
		}

		// Imports the bytes `font` as project://Assets/Fonts/<name> with `settings` (a JSON object, or null for the
		// defaults), without writing them to the project.
		Result<ImportResult> ImportFontBytes(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> font, const Json& settings)
		{
			const std::string relative = std::string("Assets/Fonts/") + std::string(name);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xf0e7000000000001ull);
			metadata.Type = AssetType::Font;
			metadata.Importer = std::string(FontImporter::Id);
			metadata.ImporterVersion = FontImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = font,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return FontImporter().Import(context, metadata);
		}

		// Imports `font` as project://Assets/Fonts/<name> with `settings`, written to the project and read back first.
		Result<ImportResult> ImportFont(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> font, const Json& settings)
		{
			const std::string relative = std::string("Assets/Fonts/") + std::string(name);
			fixture.WriteProjectFile(relative, font);
			const Buffer bytes = fixture.ReadProjectFile(relative);
			return ImportFontBytes(fixture, name, bytes, settings);
		}

		// Imports Inter with the default settings.
		Result<ImportResult> ImportInter(Test::AssetTestFixture& fixture)
		{
			Json settings = Json::object();
			settings["PixelSize"] = 48.0;
			settings["Spread"] = 8.0;
			return ImportFont(fixture, "Inter-Regular.ttf", ReadInter(), settings);
		}

		AssetRef<FontData> LoadFont(const ImportResult& result)
		{
			REQUIRE(result.Artifacts.size() == 1);
			Result<AssetRef<FontData>> font = LoadCookedFont(result.Artifacts.front().Cooked);
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			return std::move(*font);
		}

		// The kerning adjustment of (first, second), or 0 when the font lists none.
		float GetKerning(const FontData& font, uint32_t first, uint32_t second)
		{
			const auto found = std::ranges::find_if(font.Kerning, [first, second](const FontKerningPair& pair)
			{
				return pair.First == first && pair.Second == second;
			});
			return found == font.Kerning.end() ? 0.0f : found->Advance;
		}

		// The atlas value at the normalized atlas coordinate `uv`.
		uint8_t SampleAtlas(const FontData& font, glm::vec2 uv)
		{
			const uint32_t x = std::min(font.AtlasWidth - 1, static_cast<uint32_t>(uv.x * static_cast<float>(font.AtlasWidth)));
			const uint32_t y = std::min(font.AtlasHeight - 1, static_cast<uint32_t>(uv.y * static_cast<float>(font.AtlasHeight)));
			return std::to_integer<uint8_t>(font.AtlasPixels[static_cast<size_t>(y) * font.AtlasWidth + x]);
		}

		// The atlas value at the normalized atlas coordinate `uv`, interpolated bilinearly between texel centres.
		float SampleAtlasBilinear(const FontData& font, glm::vec2 uv)
		{
			const float x = uv.x * static_cast<float>(font.AtlasWidth) - 0.5f;
			const float y = uv.y * static_cast<float>(font.AtlasHeight) - 0.5f;
			const float left = std::floor(x);
			const float top = std::floor(y);
			const auto texel = [&font](float column, float row)
			{
				const uint32_t tx = static_cast<uint32_t>(std::clamp(column, 0.0f, static_cast<float>(font.AtlasWidth - 1)));
				const uint32_t ty = static_cast<uint32_t>(std::clamp(row, 0.0f, static_cast<float>(font.AtlasHeight - 1)));
				return static_cast<float>(std::to_integer<uint8_t>(font.AtlasPixels[static_cast<size_t>(ty) * font.AtlasWidth + tx]));
			};
			const float fx = x - left;
			const float fy = y - top;
			const float upper = texel(left, top) * (1.0f - fx) + texel(left + 1.0f, top) * fx;
			const float lower = texel(left, top + 1.0f) * (1.0f - fx) + texel(left + 1.0f, top + 1.0f) * fx;
			return upper * (1.0f - fy) + lower * fy;
		}

		// The distance field of `glyph` at the em-space point `plane` (y up), sampled bilinearly from the atlas.
		float SampleGlyph(const FontData& font, const FontGlyph& glyph, glm::vec2 plane)
		{
			const glm::vec2 t = (plane - glyph.PlaneMin) / (glyph.PlaneMax - glyph.PlaneMin);
			// Plane y grows up, atlas y down.
			return SampleAtlasBilinear(font, glm::vec2(glyph.AtlasMin.x + t.x * (glyph.AtlasMax.x - glyph.AtlasMin.x), glyph.AtlasMax.y - t.y * (glyph.AtlasMax.y - glyph.AtlasMin.y)));
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("FontImporter: 200 seeded mutations of TrueType and OpenType fonts never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The guard stb_truetype relies on (ValidateTrueTypeTables, which FontImporter::ValidateSource and the import's
			// TrueTypeFont::Open run) must refuse every font stb_truetype would read out of bounds or assert on. Each mutation
			// is imported from memory at the smallest settings, so a font that passes opens, answers its metrics and kerning,
			// and rasterizes every glyph of the character set it has. The seeds: Glyphs.ttf (quadratic outlines, legacy
			// kerning, five glyphs) and, one iteration in eight, the pinned Inter OTF (CFF outlines, GPOS kerning).
			const std::array<Buffer, 2> seeds = { ReadFontFixture("Glyphs.ttf"), ReadFontFixture("Inter-Regular.otf") };
			Test::AssetTestFixture fixture;
			Json settings = Json::object();
			settings["PixelSize"] = 16.0;
			settings["Spread"] = 1.0;
			const Test::FuzzRun run = Test::RunFuzzCase(0xF0A7, 200, [&seeds, &fixture, &settings](Random& random, uint64_t /*index*/)
			{
				Buffer mutated = seeds[random.RangeInt(0, 7) == 0 ? 1 : 0];
				Test::MutateBytes(mutated, random);
				const Status validated = FontImporter::ValidateSource(mutated);
				const Result<ImportResult> imported = ImportFontBytes(fixture, "Fuzz.otf", mutated, settings);
				if (!imported.has_value())
				{
					CHECK_MESSAGE(imported.error().GetCode() == ErrorCode::ImportFailed, imported.error().ToString());
					return false;
				}
				CHECK(validated.has_value());
				REQUIRE(imported->Artifacts.size() == 1);
				const Result<AssetRef<FontData>> font = LoadCookedFont(imported->Artifacts.front().Cooked);
				CHECK_MESSAGE(font.has_value(), font.error().ToString());
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("FontImporter: Inter imports ASCII and Latin-1 into an SDF atlas")
		{
			Test::AssetTestFixture fixture;
			Result<ImportResult> imported = ImportInter(fixture);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Font);
			CHECK(imported->Dependencies.empty());
			Result<AssetRef<FontData>> font = LoadCookedFont(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			CHECK((*font)->PixelSize == 48.0f);
			CHECK((*font)->Spread == 8.0f);
			for (uint32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
				CHECK(FindGlyph(**font, codepoint) != nullptr);
			CHECK(FindGlyph(**font, 0xE9) != nullptr);   // é
			CHECK(FindGlyph(**font, 0x0100) == nullptr); // outside Latin-1
			CHECK(FindGlyph(**font, 0x7F) == nullptr);   // DEL is not in the character set
			REQUIRE(FindGlyph(**font, ' ') != nullptr);
			CHECK(FindGlyph(**font, ' ')->PlaneMin == FindGlyph(**font, ' ')->PlaneMax);
			CHECK(FindGlyph(**font, ' ')->Advance > 0.0f);
			CHECK((*font)->Ascent > 0.0f);
			CHECK((*font)->Descent < 0.0f);
			CHECK((*font)->AtlasWidth == (*font)->AtlasHeight);
			CHECK(std::has_single_bit((*font)->AtlasWidth));
		}

		TEST_CASE("FontImporter: glyph rectangles match the atlas and the field is inside-positive")
		{
			Test::AssetTestFixture fixture;
			Result<ImportResult> imported = ImportInter(fixture);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<FontData> font = LoadFont(*imported);

			const FontGlyph* letter = FindGlyph(*font, 'H');
			REQUIRE(letter != nullptr);
			// Atlas and plane rectangles describe the same pixels: PixelSize atlas pixels per em.
			const glm::vec2 atlasPixels = (letter->AtlasMax - letter->AtlasMin) * glm::vec2(static_cast<float>(font->AtlasWidth), static_cast<float>(font->AtlasHeight));
			const glm::vec2 planePixels = (letter->PlaneMax - letter->PlaneMin) * font->PixelSize;
			CHECK(atlasPixels.x == doctest::Approx(planePixels.x));
			CHECK(atlasPixels.y == doctest::Approx(planePixels.y));
			// The padding holds the full spread (8 pixels): the rectangle's corner is far outside the outline, while the
			// texel 2 pixels into the left stem of 'H' (8 pixels of padding, then the stem), half way up, is inside.
			CHECK(SampleAtlas(*font, letter->AtlasMin) < 16);
			CHECK(letter->PlaneMin.y < 0.0f);
			CHECK(letter->PlaneMax.y > 0.6f);
			const glm::vec2 stem(letter->AtlasMin.x + 10.0f / static_cast<float>(font->AtlasWidth), (letter->AtlasMin.y + letter->AtlasMax.y) * 0.5f);
			CHECK(SampleAtlas(*font, stem) > 128);

			// Inter kerns 'A' before 'V' (GPOS, inside an extension lookup) and never 'H' before 'H'.
			CHECK(GetKerning(*font, 'A', 'V') < 0.0f);
			CHECK(GetKerning(*font, 'H', 'H') == 0.0f);
			CHECK(std::ranges::is_sorted(font->Kerning, {}, [](const FontKerningPair& pair)
			{
				return std::pair(pair.First, pair.Second);
			}));
			CHECK(font->Kerning.size() > 100);
		}

		TEST_CASE("FontImporter: the atlas is deterministic and invalid fonts fail")
		{
			Test::AssetTestFixture fixture;
			Result<ImportResult> first = ImportInter(fixture);
			Result<ImportResult> second = ImportInter(fixture);
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(first->Artifacts.front().Cooked == second->Artifacts.front().Cooked);

			fixture.WriteProjectText("Assets/Fonts/Broken.ttf", "not a font");
			const Buffer broken = fixture.ReadProjectFile("Assets/Fonts/Broken.ttf");
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xf0e7000000000002ull);
			metadata.Type = AssetType::Font;
			metadata.Importer = std::string(FontImporter::Id);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Fonts/Broken.ttf"),
				.SourceBytes = broken,
				.Settings = VariantValue(Json::object()),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			Result<ImportResult> failed = FontImporter().Import(context, metadata);
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::ImportFailed);
		}

		TEST_CASE("FontImporter: truncated and damaged fonts fail with ImportFailed instead of being read out of bounds")
		{
			Test::AssetTestFixture fixture;
			const Buffer inter = ReadInter();
			const auto expectFailure = [&fixture](std::string_view name, std::span<const std::byte> bytes)
			{
				CAPTURE(std::string(name));
				Result<ImportResult> result = ImportFont(fixture, name, bytes, Json::object());
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().GetCode() == ErrorCode::ImportFailed);
				CHECK(result.error().ToString().find(name) != std::string::npos);
			};

			// Cut inside the table directory, and inside the last table.
			expectFailure("Header.ttf", std::span<const std::byte>(inter).first(40));
			expectFailure("Truncated.ttf", std::span<const std::byte>(inter).first(inter.size() / 2));

			// A table directory claiming far more tables than the file holds.
			Buffer tables = inter;
			tables[4] = std::byte{ 0x7F };
			expectFailure("Tables.ttf", tables);

			// A font collection signature.
			Buffer collection = inter;
			collection[0] = std::byte{ 't' };
			collection[1] = std::byte{ 't' };
			collection[2] = std::byte{ 'c' };
			collection[3] = std::byte{ 'f' };
			expectFailure("Collection.ttf", collection);
		}

		TEST_CASE("FontImporter: glyph outlines stb_truetype would read past, recurse into or assert on are refused at validation")
		{
			// Found by the FontImporter fuzz case under AddressSanitizer (Docs/Decisions/0024-m15-contract.md decision 13):
			// stb_truetype reads a glyph's flags and coordinates, a composite glyph's components and the point after a contour's
			// first without bounds checks. Each damaged Glyphs.ttf below passed validation before the outline checks; now
			// validation refuses it, so stb_truetype never reads it.
			const Buffer original = ReadFontFixture("Glyphs.ttf");
			REQUIRE(FontImporter::ValidateSource(original).has_value());
			// Glyphs.ttf: 5 glyphs with 16-bit locations; glyph 3 (two contours, 96 bytes) and glyph 4 (one contour, 44 bytes,
			// the last) are simple glyphs.
			const auto readU16 = [](std::span<const std::byte> bytes, size_t offset)
			{
				return static_cast<uint32_t>((std::to_integer<uint32_t>(bytes[offset]) << 8) | std::to_integer<uint32_t>(bytes[offset + 1]));
			};
			const auto findTable = [&original, &readU16](std::string_view tag)
			{
				const uint32_t tableCount = readU16(original, 4);
				for (uint32_t table = 0; table < tableCount; ++table)
				{
					const size_t record = 12 + 16 * table;
					if (std::memcmp(original.data() + record, tag.data(), 4) == 0)
						return static_cast<size_t>((readU16(original, record + 8) << 16) | readU16(original, record + 10));
				}
				FAIL("Glyphs.ttf has no '", std::string(tag), "' table");
				return size_t{ 0 };
			};
			const size_t glyf = findTable("glyf");
			const size_t loca = findTable("loca");
			const size_t head = findTable("head");
			// Glyphs.ttf with the start of glyph `glyph`'s description overwritten by `description` (its length stays).
			const auto patchGlyph = [&original, &readU16, glyf, loca](uint32_t glyph, std::initializer_list<uint8_t> description)
			{
				Buffer font = original;
				const size_t begin = glyf + 2 * readU16(original, loca + 2 * glyph);
				const size_t end = glyf + 2 * readU16(original, loca + 2 * glyph + 2);
				REQUIRE(description.size() <= end - begin);
				std::ranges::transform(description, font.begin() + static_cast<std::ptrdiff_t>(begin), [](uint8_t value)
				{
					return std::byte{ value };
				});
				return font;
			};
			Test::AssetTestFixture fixture;
			const auto expectRefused = [&fixture](std::string_view what, const Buffer& font, std::string_view message)
			{
				CAPTURE(std::string(what));
				const Status validated = FontImporter::ValidateSource(font);
				REQUIRE_FALSE(validated.has_value());
				CHECK(validated.error().GetCode() == ErrorCode::ImportFailed);
				CHECK(validated.error().GetMessageText().contains(message));
				const Result<ImportResult> imported = ImportFontBytes(fixture, "Damaged.ttf", font, Json::object());
				REQUIRE_FALSE(imported.has_value());
				CHECK(imported.error().GetMessageText().contains(message));
			};

			// One contour of 201 points, whose flags and coordinates the 44-byte glyph cannot hold.
			expectRefused("truncated points", patchGlyph(4, { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 200, 0, 0 }), "glyph 4");
			// One contour of a single off-curve point: stb_truetype reads the point after it, past its outline.
			expectRefused("one off-curve point", patchGlyph(4, { 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0, 1, 0, 1 }),
				"one off-curve point");
			// Contour end points that do not increase.
			expectRefused("decreasing contours", patchGlyph(3, { 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 3 }), "do not increase");
			// A composite glyph whose component is itself: stb_truetype would recurse without end.
			expectRefused("component cycle", patchGlyph(3, { 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x02, 0, 3, 0, 0 }), "cycle");
			// A composite glyph placed by matching points, which stb_truetype asserts on.
			expectRefused("point matching", patchGlyph(3, { 0xFF, 0xFF, 0, 0, 0, 0, 0, 0, 0, 0, 0x00, 0x00, 0, 0, 0, 0 }), "matching points");
			// One unit per em: a glyph's pixel size would not be bounded.
			Buffer units = original;
			units[head + 18] = std::byte{ 0 };
			units[head + 19] = std::byte{ 1 };
			expectRefused("one unit per em", units, "units per em");
		}

		TEST_CASE("FontImporter: a glyph box more than 2048 pixels on a side is refused before its distance field is allocated")
		{
			// TrueTypeFont::MaxGlyphBitmapSide: stb_truetype sizes a TrueType glyph's bitmap from the bounding box in its glyf
			// header, which validation does not bound, and the distance field costs a byte and a solve for every pixel, so a
			// header claiming a huge box would make the import allocate and solve billions of pixels. Glyphs.ttf here gets 1024
			// units per em, so at PixelSize 128 a unit is exactly 1/8 pixel and Spread 1 pads each side by one pixel, and glyph
			// 3 ('O', a box of 50,0 to 550,700 units) gets the box below: its field is ceil(xMax / 8) - floor(xMin / 8) + 2
			// pixels wide and ceil(-yMin / 8) - floor(-yMax / 8) + 2 tall.
			const Buffer original = ReadFontFixture("Glyphs.ttf");
			const auto readU16 = [](std::span<const std::byte> bytes, size_t offset)
			{
				return static_cast<uint32_t>((std::to_integer<uint32_t>(bytes[offset]) << 8) | std::to_integer<uint32_t>(bytes[offset + 1]));
			};
			const auto findTable = [&original, &readU16](std::string_view tag)
			{
				for (uint32_t table = 0; table < readU16(original, 4); ++table)
				{
					const size_t record = 12 + 16 * table;
					if (std::memcmp(original.data() + record, tag.data(), 4) == 0)
						return static_cast<size_t>((readU16(original, record + 8) << 16) | readU16(original, record + 10));
				}
				FAIL("Glyphs.ttf has no '", std::string(tag), "' table");
				return size_t{ 0 };
			};
			const size_t glyph3 = findTable("glyf") + 2 * readU16(original, findTable("loca") + 2 * 3);
			const size_t head = findTable("head");
			REQUIRE(readU16(original, glyph3 + 2) == 50);
			REQUIRE(readU16(original, glyph3 + 6) == 550);
			REQUIRE(readU16(original, glyph3 + 8) == 700);
			const auto withBox = [&original, glyph3, head](int16_t xMin, int16_t yMin, int16_t xMax, int16_t yMax)
			{
				Buffer font = original;
				font[head + 18] = std::byte{ 0x04 }; // 1024 units per em
				font[head + 19] = std::byte{ 0x00 };
				size_t offset = glyph3 + 2;
				for (const int16_t value : { xMin, yMin, xMax, yMax })
				{
					const auto bits = static_cast<uint16_t>(value);
					font[offset++] = static_cast<std::byte>(bits >> 8);
					font[offset++] = static_cast<std::byte>(bits & 0xFF);
				}
				return font;
			};
			Test::AssetTestFixture fixture;
			Json settings = Json::object();
			settings["PixelSize"] = 128.0;
			settings["Spread"] = 1.0;

			// Exactly 2048 pixels wide (0 to 2046, padded): imported, at that size.
			const Result<ImportResult> widest = ImportFontBytes(fixture, "Widest.ttf", withBox(0, 0, 16368, 8), settings);
			REQUIRE_MESSAGE(widest.has_value(), widest.error().ToString());
			const AssetRef<FontData> font = LoadFont(*widest);
			const FontGlyph* letter = FindGlyph(*font, 'O');
			REQUIRE(letter != nullptr);
			CHECK((letter->AtlasMax.x - letter->AtlasMin.x) * static_cast<float>(font->AtlasWidth) == doctest::Approx(2048.0f));

			// One pixel wider, then one pixel taller than that: refused with the import's location, before stb_truetype reads
			// the outline (REQUIRE: without the check the next case would solve a field of 8192 x 8192 pixels).
			const auto expectRefused = [&fixture, &settings](std::string_view name, const Buffer& bytes, std::string_view message)
			{
				CAPTURE(std::string(name));
				const Result<ImportResult> imported = ImportFontBytes(fixture, name, bytes, settings);
				REQUIRE_FALSE(imported.has_value());
				CHECK(imported.error().GetCode() == ErrorCode::ImportFailed);
				CHECK_MESSAGE(imported.error().GetMessageText().contains(message), imported.error().ToString());
				CHECK(imported.error().ToString().contains(name));
				CHECK(imported.error().ToString().contains("U+004F"));
			};
			expectRefused("Wider.ttf", withBox(0, 0, 16376, 8), "glyph 3 would be a 2049x3 pixel distance field; at most 2048 pixels on a side");
			expectRefused("Taller.ttf", withBox(0, -16376, 8, 0), "glyph 3 would be a 3x2049 pixel distance field");
			// The whole 16-bit range of the header.
			expectRefused("Huge.ttf", withBox(-32768, -32768, 32767, 32767), "glyph 3 would be a 8194x8194 pixel distance field");
		}

		TEST_CASE("FontImporter: CFF structures and charstrings stb_truetype would read past or assert on are refused at validation")
		{
			// Found by the FontImporter fuzz case (Docs/Decisions/0024-m15-contract.md decision 13): stb_truetype views a CFF
			// table through a 512 MiB buffer, asserts on INDEX and DICT encodings and on a hint mask past the end of a
			// charstring, reads the vertex before an outline that starts with a line, and runs nested subroutine calls without
			// a limit on their work. Each damaged Inter OTF below passed validation before the CFF checks; now validation (and
			// so the import) refuses it, so stb_truetype never reads it.
			const Buffer original = ReadFontFixture("Inter-Regular.otf");
			REQUIRE(FontImporter::ValidateSource(original).has_value());
			const auto byteAt = [](std::span<const std::byte> bytes, size_t offset)
			{
				return std::to_integer<uint32_t>(bytes[offset]);
			};
			const auto readU16 = [&byteAt](std::span<const std::byte> bytes, size_t offset)
			{
				return (byteAt(bytes, offset) << 8) | byteAt(bytes, offset + 1);
			};
			const auto readU32 = [&readU16](std::span<const std::byte> bytes, size_t offset)
			{
				return (readU16(bytes, offset) << 16) | readU16(bytes, offset + 2);
			};
			const auto writeBytes = [](Buffer& bytes, size_t offset, std::initializer_list<uint8_t> values)
			{
				std::ranges::transform(values, bytes.begin() + static_cast<std::ptrdiff_t>(offset), [](uint8_t value)
				{
					return std::byte{ value };
				});
			};
			size_t cff = 0;
			for (uint32_t table = 0; table < readU16(original, 4); ++table)
			{
				const size_t record = 12 + 16 * table;
				if (std::memcmp(original.data() + record, "CFF ", 4) == 0)
					cff = readU32(original, record + 8);
			}
			REQUIRE(cff != 0);
			// Element `element` of the CFF INDEX at `index` (relative to the table): its first byte in the file and its size.
			const auto indexElement = [&original, &byteAt, &readU16, cff](size_t index, uint32_t element)
			{
				const size_t at = cff + index;
				const uint32_t count = readU16(original, at);
				const uint32_t offsetSize = byteAt(original, at + 2);
				REQUIRE(element < count);
				const auto offset = [&](uint32_t which)
				{
					size_t value = 0;
					for (uint32_t digit = 0; digit < offsetSize; ++digit)
						value = (value << 8) | byteAt(original, at + 3 + which * offsetSize + digit);
					return value;
				};
				const size_t data = at + 3 + (count + 1) * offsetSize - 1;
				return std::pair{ data + offset(element), offset(element + 1) - offset(element) };
			};
			Test::AssetTestFixture fixture;
			// Each sub-case is checked on its own (a failed one does not stop the others), and a font that validation lets
			// through is never imported, since stb_truetype would read past or assert on it.
			const auto expectRefused = [&fixture](std::string_view what, const Buffer& font, std::string_view message)
			{
				CAPTURE(std::string(what));
				const Status validated = FontImporter::ValidateSource(font);
				CHECK_FALSE(validated.has_value());
				if (validated.has_value())
					return;
				CHECK(validated.error().GetCode() == ErrorCode::ImportFailed);
				CHECK_MESSAGE(validated.error().GetMessageText().contains(message), validated.error().ToString());
				const Result<ImportResult> imported = ImportFontBytes(fixture, "Damaged.otf", font, Json::object());
				CHECK_FALSE(imported.has_value());
				if (!imported.has_value())
					CHECK(imported.error().GetMessageText().contains(message));
			};

			// Inter-Regular.otf's CFF table: a 4-byte header and an 18-byte Name INDEX at 4 (one 13-byte name, offset size 1),
			// then the Top DICT INDEX at 22 with one 84-byte Top DICT at 27, the String INDEX, and the Global Subr INDEX at 29945
			// (490 subroutines, bias 107). The Top DICT holds UnderlinePosition (the two-byte operand 251 240, operator 12 3) at
			// 48 and UnderlineThickness (the two-byte operand 247 32, operator 12 4) at 52, operands nothing but the DICT
			// parser reads, and ends with Private (size 34 as one byte, a 32-bit offset, operator 18) at 98 and CharStrings (a
			// 32-bit offset, operator 17) at 105; the Private DICT ends with Subrs (34 as one byte, operator 19).
			REQUIRE(readU16(original, cff + 4) == 1);
			REQUIRE(byteAt(original, cff + 6) == 1);
			REQUIRE(byteAt(original, cff + 8) == 14);
			REQUIRE(readU16(original, cff + 22) == 1);
			REQUIRE(byteAt(original, cff + 48) == 251);
			REQUIRE(byteAt(original, cff + 50) == 12);
			REQUIRE(byteAt(original, cff + 51) == 3);
			REQUIRE(byteAt(original, cff + 52) == 247);
			REQUIRE(byteAt(original, cff + 54) == 12);
			REQUIRE(byteAt(original, cff + 55) == 4);
			REQUIRE(readU16(original, cff + 29945) == 490);
			REQUIRE(byteAt(original, cff + 98) == 0xAD);
			REQUIRE(byteAt(original, cff + 99) == 29);
			REQUIRE(byteAt(original, cff + 104) == 18);
			REQUIRE(byteAt(original, cff + 105) == 29);
			REQUIRE(byteAt(original, cff + 110) == 17);
			const size_t privateDict = cff + readU32(original, cff + 100);
			REQUIRE(byteAt(original, privateDict + 32) == 0xAD);
			REQUIRE(byteAt(original, privateDict + 33) == 19);
			const size_t charStrings = readU32(original, cff + 106);

			// No Top DICT: stb_truetype asserts on reading element 0 of an empty INDEX. The check gives the message; without it
			// the empty Top DICT has no CharStrings and is refused all the same (defense in depth, TrueTypeTables.cpp).
			Buffer noTopDict = original;
			writeBytes(noTopDict, cff + 22, { 0, 0 });
			expectRefused("no Top DICT", noTopDict, "no Top DICT");
			// The Name INDEX rewritten with the offset size 5 and the same length (one 5-byte name, "Inter", between the 5-byte
			// offsets 1 and 6), so the Top DICT INDEX stays at 22 and every other check passes: stb_truetype asserts on an offset
			// size outside 1 to 4, and only the offset-size check refuses this font.
			Buffer offsetSize = original;
			writeBytes(offsetSize, cff + 4, { 0, 1, 5, 0, 0, 0, 0, 1, 0, 0, 0, 0, 6, 'I', 'n', 't', 'e', 'r' });
			expectRefused("offset size", offsetSize, "Name INDEX has offset size 5");
			// CharStrings given as a real number (30, nibbles ending in 0xF), which stb_truetype reads as an integer.
			Buffer realOperand = original;
			writeBytes(realOperand, cff + 105, { 30 });
			writeBytes(realOperand, cff + 109, { 0x3F });
			expectRefused("real operand", realOperand, "real number");
			// CharStrings far outside the table, inside stb_truetype's 512 MiB view of it.
			Buffer outside = original;
			writeBytes(outside, cff + 106, { 0x07, 0xFF, 0xFF, 0xF0 });
			expectRefused("CharStrings outside", outside, "offset 134217712");
			// The reserved operand bytes in Top DICT operands that only the DICT parser reads, so every other check passes:
			// stb_truetype asserts on either byte when it skips the operands before the entry it looks up, and only the
			// reserved-byte check refuses these fonts. UnderlineThickness's operand becomes 31 (sized as one byte) and the
			// one-byte operand 0; UnderlinePosition's two-byte operand starts with 255 (sized as two bytes).
			Buffer reserved31 = original;
			writeBytes(reserved31, cff + 52, { 31, 139 });
			expectRefused("reserved operand 31", reserved31, "reserved operand byte 31");
			Buffer reserved255 = original;
			writeBytes(reserved255, cff + 48, { 255 });
			expectRefused("reserved operand 255", reserved255, "reserved operand byte 255");

			// Glyph 0's charstring replaced by stem hints (two operands and hstem, over and over) and a hint mask as its last
			// byte, whose mask bytes would lie past its end.
			const auto [glyph0, glyph0Size] = indexElement(charStrings, 0);
			REQUIRE(glyph0Size > 4);
			Buffer hintMask = original;
			for (size_t index = 0; index + 1 < glyph0Size; ++index)
				writeBytes(hintMask, glyph0 + index, { index % 3 == 2 ? uint8_t{ 0x01 } : uint8_t{ 0x8B } });
			writeBytes(hintMask, glyph0 + glyph0Size - 1, { 0x13 });
			expectRefused("hint mask", hintMask, "hint mask");
			// Glyph 0's charstring drawing a line (0 0 rlineto) before any moveto: its outline would start with a line, and
			// stb_truetype's winding test reads the vertex before every line.
			Buffer lineFirst = original;
			writeBytes(lineFirst, glyph0, { 0x8B, 0x8B, 0x05, 0x0E });
			expectRefused("line before moveto", lineFirst, "draws before its first moveto");

			// Nine global subroutines each calling the next four times (the last just returns), called by glyph 0: 4^9 calls
			// within stb_truetype's nesting limit of 10.
			Buffer nested = original;
			std::vector<uint32_t> chain;
			for (uint32_t element = 0; element <= 214 && chain.size() < 9; ++element)
			{
				if (indexElement(29945, element).second >= 9)
					chain.push_back(element);
			}
			REQUIRE(chain.size() == 9);
			// A one-byte operand pushing the subroutine number of element `element` (bias 107).
			const auto pushSubroutine = [](uint32_t element)
			{
				return static_cast<uint8_t>(static_cast<int32_t>(element) - 107 + 139);
			};
			for (size_t link = 0; link < chain.size(); ++link)
			{
				const size_t subroutine = indexElement(29945, chain[link]).first;
				if (link + 1 == chain.size())
				{
					writeBytes(nested, subroutine, { 0x0B });
					continue;
				}
				const uint8_t next = pushSubroutine(chain[link + 1]);
				writeBytes(nested, subroutine, { next, 0x1D, next, 0x1D, next, 0x1D, next, 0x1D, 0x0B });
			}
			writeBytes(nested, glyph0, { pushSubroutine(chain.front()), 0x1D, 0x0E });
			expectRefused("nested subroutines", nested, "more than 65536 operators");
		}

		TEST_CASE("FontImporter: settings change the atlas resolution and spread")
		{
			Test::AssetTestFixture fixture;
			Json small = Json::object();
			small["PixelSize"] = 16.0;
			small["Spread"] = 2.0;
			Result<ImportResult> imported = ImportFont(fixture, "Small.ttf", ReadInter(), small);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<FontData> font = LoadFont(*imported);
			CHECK(font->PixelSize == 16.0f);
			CHECK(font->Spread == 2.0f);

			Result<ImportResult> defaults = ImportInter(fixture);
			REQUIRE(defaults.has_value());
			const AssetRef<FontData> large = LoadFont(*defaults);
			CHECK(font->AtlasWidth < large->AtlasWidth);
			// Metrics in em units do not depend on the resolution.
			CHECK(font->Ascent == large->Ascent);
			CHECK(FindGlyph(*font, 'W')->Advance == FindGlyph(*large, 'W')->Advance);

			Json invalid = Json::object();
			invalid["PixelSize"] = 4.0;
			Result<ImportResult> rejected = ImportFont(fixture, "Tiny.ttf", ReadInter(), invalid);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("FontImporter: the generated Glyphs.ttf imports its quadratic outlines, counters and legacy kerning")
		{
			// Tests/Data/Assets/Fonts/Glyphs.ttf (MakeFormatFixtures.py): 1000 units per em, ascent 800, descent -200; space
			// (advance 250, no outline), A and V (advance 600, straight contours, A with a counter), O (advance 600, quadratic
			// curves around a counter); its kern table (format 0) kerns A-V by -80 and V-A by -60 units.
			Test::AssetTestFixture fixture;
			Json settings = Json::object();
			settings["PixelSize"] = 48.0;
			settings["Spread"] = 8.0;
			Result<ImportResult> imported = ImportFont(fixture, "Glyphs.ttf", ReadFontFixture("Glyphs.ttf"), settings);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<FontData> font = LoadFont(*imported);
			CHECK(font->Ascent == doctest::Approx(0.8f));
			CHECK(font->Descent == doctest::Approx(-0.2f));
			CHECK(font->LineGap == 0.0f);
			REQUIRE(font->Glyphs.size() == 4);
			CHECK(FindGlyph(*font, 'B') == nullptr);
			const FontGlyph* space = FindGlyph(*font, ' ');
			REQUIRE(space != nullptr);
			CHECK(space->Advance == doctest::Approx(0.25f));
			CHECK(space->PlaneMin == space->PlaneMax);
			for (const uint32_t codepoint : { uint32_t{ 'A' }, uint32_t{ 'O' }, uint32_t{ 'V' } })
			{
				CAPTURE(codepoint);
				const FontGlyph* glyph = FindGlyph(*font, codepoint);
				REQUIRE(glyph != nullptr);
				CHECK(glyph->Advance == doctest::Approx(0.6f));
				// The outlines span about 0 to 0.7 em in height and 0.5 em or more in width, plus the padding.
				CHECK(glyph->PlaneMax.y - glyph->PlaneMin.y > 0.7f);
				CHECK(glyph->PlaneMax.x - glyph->PlaneMin.x > 0.5f);
			}
			// Inside-positive: the middle of O's ring (x 100 of 50..150, y 350) is inside, its counter's centre outside.
			const FontGlyph* letter = FindGlyph(*font, 'O');
			REQUIRE(letter != nullptr);
			const auto atlasAt = [&font, letter](float emX, float emY)
			{
				const glm::vec2 plane(emX, emY);
				const glm::vec2 t = (plane - letter->PlaneMin) / (letter->PlaneMax - letter->PlaneMin);
				// Plane y grows up, atlas y down.
				return SampleAtlas(*font, glm::vec2(letter->AtlasMin.x + t.x * (letter->AtlasMax.x - letter->AtlasMin.x), letter->AtlasMax.y - t.y * (letter->AtlasMax.y - letter->AtlasMin.y)));
			};
			CHECK(atlasAt(0.1f, 0.35f) > 128);
			CHECK(atlasAt(0.3f, 0.35f) < 128);
			CHECK(GetKerning(*font, 'A', 'V') == doctest::Approx(-0.08f));
			CHECK(GetKerning(*font, 'V', 'A') == doctest::Approx(-0.06f));
			CHECK(GetKerning(*font, 'A', 'O') == 0.0f);
			CHECK(font->Kerning.size() == 2);
		}

		TEST_CASE("FontImporter: the pinned Inter OTF imports through its CFF outlines")
		{
			// Tests/Data/Assets/Fonts/Inter-Regular.otf (FetchAssets.py fixture): the same Inter 4.1 design as the engine's
			// TrueType font, with CFF outlines ('OTTO'), so the metrics, character set and kerning agree with it.
			Test::AssetTestFixture fixture;
			Json settings = Json::object();
			settings["PixelSize"] = 48.0;
			settings["Spread"] = 8.0;
			const Buffer otf = ReadFontFixture("Inter-Regular.otf");
			REQUIRE(otf.size() > 4);
			CHECK(std::to_integer<char>(otf[0]) == 'O');
			CHECK(std::to_integer<char>(otf[1]) == 'T');
			CHECK(std::to_integer<char>(otf[2]) == 'T');
			CHECK(std::to_integer<char>(otf[3]) == 'O');
			Result<ImportResult> imported = ImportFont(fixture, "Inter-Regular.otf", otf, settings);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<FontData> font = LoadFont(*imported);
			Result<ImportResult> truetype = ImportInter(fixture);
			REQUIRE(truetype.has_value());
			const AssetRef<FontData> reference = LoadFont(*truetype);
			CHECK(font->Glyphs.size() == reference->Glyphs.size());
			CHECK(font->Ascent == doctest::Approx(reference->Ascent));
			CHECK(font->Descent == doctest::Approx(reference->Descent));
			for (uint32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
			{
				CAPTURE(codepoint);
				const FontGlyph* glyph = FindGlyph(*font, codepoint);
				REQUIRE(glyph != nullptr);
				CHECK(glyph->Advance == doctest::Approx(FindGlyph(*reference, codepoint)->Advance));
			}
			CHECK(GetKerning(*font, 'A', 'V') < 0.0f);
			CHECK(GetKerning(*font, 'A', 'V') == doctest::Approx(GetKerning(*reference, 'A', 'V')));
			// The CFF outline rasterizes inside-positive like the TrueType one: the stem of 'H' is inside.
			const FontGlyph* letter = FindGlyph(*font, 'H');
			REQUIRE(letter != nullptr);
			const glm::vec2 stem(letter->AtlasMin.x + 10.0f / static_cast<float>(font->AtlasWidth), (letter->AtlasMin.y + letter->AtlasMax.y) * 0.5f);
			CHECK(SampleAtlas(*font, stem) > 128);
		}

		// ADR 0022 "Integration": stb_truetype's distance-field solver skips cubic segments, which only CFF outlines have, so
		// every curved .otf glyph used to import garbled ('Inter OTF' rendered as 'Int_r TF'). The rasterizer now replaces
		// cubics by quadratics before the solver, so the CFF and the TrueType outlines of the same design give nearly the
		// same field: compared at the same em-space points, they differ by a few levels at most (the TrueType outlines are
		// themselves a quadratic approximation of the cubic masters), where a dropped segment moves the field by its whole
		// range.
		TEST_CASE("FontImporter: the OTF's cubic outlines rasterize like the TrueType outlines of the same design")
		{
			Test::AssetTestFixture fixture;
			Json settings = Json::object();
			settings["PixelSize"] = 48.0;
			settings["Spread"] = 8.0;
			Result<ImportResult> imported = ImportFont(fixture, "Inter-Regular.otf", ReadFontFixture("Inter-Regular.otf"), settings);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			const AssetRef<FontData> font = LoadFont(*imported);
			Result<ImportResult> truetype = ImportInter(fixture);
			REQUIRE(truetype.has_value());
			const AssetRef<FontData> reference = LoadFont(*truetype);
			constexpr int Samples = 24;
			float worst = 0.0f;
			double total = 0.0;
			uint32_t count = 0;
			for (uint32_t codepoint = 0x21; codepoint <= 0x7E; ++codepoint)
			{
				CAPTURE(codepoint);
				const FontGlyph* glyph = FindGlyph(*font, codepoint);
				const FontGlyph* expected = FindGlyph(*reference, codepoint);
				REQUIRE(glyph != nullptr);
				REQUIRE(expected != nullptr);
				// The same outline, so the same box within a pixel (1/48 em).
				CHECK(glm::all(glm::lessThanEqual(glm::abs(glyph->PlaneMin - expected->PlaneMin), glm::vec2(1.0f / 48.0f + 1e-4f))));
				CHECK(glm::all(glm::lessThanEqual(glm::abs(glyph->PlaneMax - expected->PlaneMax), glm::vec2(1.0f / 48.0f + 1e-4f))));
				const glm::vec2 low = glm::max(glyph->PlaneMin, expected->PlaneMin);
				const glm::vec2 high = glm::min(glyph->PlaneMax, expected->PlaneMax);
				float glyphWorst = 0.0f;
				for (int row = 0; row < Samples; ++row)
				{
					for (int column = 0; column < Samples; ++column)
					{
						const glm::vec2 t((static_cast<float>(column) + 0.5f) / Samples, (static_cast<float>(row) + 0.5f) / Samples);
						const glm::vec2 plane = low + t * (high - low);
						const float difference = std::abs(SampleGlyph(*font, *glyph, plane) - SampleGlyph(*reference, *expected, plane));
						glyphWorst = std::max(glyphWorst, difference);
						total += difference;
						++count;
					}
				}
				// Measured: at most one level (rounding and interpolation) for every printable ASCII glyph.
				CHECK(glyphWorst <= 4.0f);
				worst = std::max(worst, glyphWorst);
			}
			CAPTURE(worst);
			CHECK(total / count < 0.25);
			// 'O' as the report that found the defect described it: across its middle row the field rises above the edge
			// value in the left half of the ring and falls below it in the counter.
			const FontGlyph* letter = FindGlyph(*font, 'O');
			REQUIRE(letter != nullptr);
			const float middle = (letter->PlaneMin.y + letter->PlaneMax.y) * 0.5f;
			const float centre = (letter->PlaneMin.x + letter->PlaneMax.x) * 0.5f;
			float ring = 0.0f;
			for (int step = 0; step <= Samples; ++step)
				ring = std::max(ring, SampleGlyph(*font, *letter, glm::vec2(letter->PlaneMin.x + (centre - letter->PlaneMin.x) * static_cast<float>(step) / Samples, middle)));
			CHECK(ring > 128.0f);
			CHECK(SampleGlyph(*font, *letter, glm::vec2(centre, middle)) < 128.0f);
		}
	}

}
