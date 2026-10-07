#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/FontImporter.h"

#include "Engine/Asset/FontData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// Imports the committed Inter-Regular.ttf (Resources/Fonts, SIL OFL) with the default settings.
		Result<ImportResult> ImportInter(Test::AssetTestFixture& fixture)
		{
			Result<Buffer> font = FileSystem::ReadFile(Test::GetRepositoryRoot() / "Resources/Fonts/Inter-Regular.ttf");
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			fixture.WriteProjectFile("Assets/Fonts/Inter-Regular.ttf", *font);
			Json settings = Json::object();
			settings["PixelSize"] = 48.0;
			settings["Spread"] = 8.0;
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0xf0e7000000000001ull);
			metadata.Type = AssetType::Font;
			metadata.Importer = std::string(FontImporter::Id);
			metadata.ImporterVersion = FontImporter::Version;
			metadata.Settings = VariantValue(settings);
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Fonts/Inter-Regular.ttf"),
				.SourceBytes = *font,
				.Settings = VariantValue(settings),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			return FontImporter().Import(context, metadata);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("FontImporter: Inter imports ASCII and Latin-1 into an SDF atlas" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			Result<ImportResult> imported = ImportInter(fixture);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			Result<AssetRef<FontData>> font = LoadCookedFont(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(font.has_value(), font.error().ToString());
			CHECK((*font)->PixelSize == 48.0f);
			CHECK((*font)->Spread == 8.0f);
			for (uint32_t codepoint = 0x20; codepoint <= 0x7E; ++codepoint)
				CHECK(FindGlyph(**font, codepoint) != nullptr);
			CHECK(FindGlyph(**font, 0xE9) != nullptr);   // é
			CHECK(FindGlyph(**font, 0x0100) == nullptr); // outside Latin-1
			REQUIRE(FindGlyph(**font, ' ') != nullptr);
			CHECK(FindGlyph(**font, ' ')->PlaneMin == FindGlyph(**font, ' ')->PlaneMax);
			CHECK((*font)->Ascent > 0.0f);
			CHECK((*font)->Descent < 0.0f);
			CHECK((*font)->AtlasWidth == (*font)->AtlasHeight);
		}

		TEST_CASE("FontImporter: the atlas is deterministic and invalid fonts fail" * doctest::skip(true))
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
	}

}
