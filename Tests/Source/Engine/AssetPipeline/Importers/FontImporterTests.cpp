#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>

namespace Engine {

	namespace {

		// The committed Inter-Regular.ttf (Resources/Fonts, SIL OFL).
		Buffer ReadInter()
		{
			Result<Buffer> font = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf");
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
	}

}
