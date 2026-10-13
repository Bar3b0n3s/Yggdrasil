#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
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

		// Imports `font` as project://Assets/Fonts/<name> with `settings` (a JSON object, or null for the defaults).
		Result<ImportResult> ImportFont(Test::AssetTestFixture& fixture, std::string_view name, std::span<const std::byte> font, const Json& settings)
		{
			const std::string relative = std::string("Assets/Fonts/") + std::string(name);
			fixture.WriteProjectFile(relative, font);
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xf0e7000000000001ull);
			metadata.Type = AssetType::Font;
			metadata.Importer = std::string(FontImporter::Id);
			metadata.ImporterVersion = FontImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = bytes,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return FontImporter().Import(context, metadata);
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
