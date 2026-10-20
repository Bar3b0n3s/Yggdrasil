#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/SpriteSheetImporter.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

// SpriteSheetImporter (M18, C18.1, R-2D-SHEET; Docs/Decisions/0029-m18-contract.md decision 17). Skipped until stream S
// implements the importer and registers it with its source format.

namespace Engine {

	namespace {

		// A sheet over the texture 0x7e57000000000001 with two frames, the second sliced, and one clip.
		constexpr std::string_view TilesSheet = R"({"Format": "SpriteSheet", "Version": 1, "Texture": "7e57000000000001",
			"Frames": [{"Name": "Floor", "Rect": {"X": 0, "Y": 0, "Width": 8, "Height": 8}, "Pivot": [0.5, 0.5]},
				{"Name": "Panel", "Rect": {"X": 8, "Y": 0, "Width": 8, "Height": 8}, "Borders": {"Left": 2, "Top": 2, "Right": 2, "Bottom": 2}}],
			"Clips": [{"Name": "Flicker", "Frames": [{"Frame": 0, "Ticks": 4}, {"Frame": 1, "Ticks": 4}], "Loop": true}]})";

		Result<ImportResult> ImportSpriteSheet(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x5b5e000000000001ull);
			metadata.Type = AssetType::SpriteSheet;
			metadata.Importer = std::string(SpriteSheetImporter::Id);
			metadata.ImporterVersion = SpriteSheetImporter::Version;
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath(relative),
				.SourceBytes = bytes,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			Result<ImportResult> result = SpriteSheetImporter().Import(context, metadata);
			// The importer reads nothing but its source: never the texture.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("SpriteSheetImporter: a sheet imports its frames, pivots, borders and clips with its texture as the dependency" * doctest::skip(true))
		{
			CHECK(SpriteSheetImporter().CanImport(".SpriteSheet"));
			CHECK(SpriteSheetImporter().GetMainType() == AssetType::SpriteSheet);
			CHECK(SpriteSheetImporter().GetSettingsTypeName().empty());
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Tiles.spritesheet", TilesSheet);
			const Result<ImportResult> imported = ImportSpriteSheet(fixture, "Assets/Tiles.spritesheet");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::SpriteSheet);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x5b5e000000000001ull));
			CHECK(imported->Artifacts.front().SubAssetKey.empty());
			// The texture is the sheet's dependency (ADR 0028's phase-1 ledger), although it is not registered: an unregistered
			// texture is the validator's ASSET_MISSING, never an import error.
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x7e57000000000001ull) });
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<SpriteSheetData>> sheet = LoadCookedSpriteSheet(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(sheet.has_value(), sheet.error().ToString());
			REQUIRE((*sheet)->Frames.size() == 2);
			CHECK((*sheet)->Frames[1].Rect == SpriteRect{ .X = 8, .Y = 0, .Width = 8, .Height = 8 });
			CHECK((*sheet)->Frames[1].Borders == SpriteBorders{ .Left = 2, .Top = 2, .Right = 2, .Bottom = 2 });
			CHECK((*sheet)->Frames[1].Pivot == glm::vec2(0.5f, 0.5f));
			REQUIRE((*sheet)->Clips.size() == 1);
			CHECK((*sheet)->Clips[0].Name == "Flicker");
			CHECK((*sheet)->Clips[0].Frames.size() == 2);

			// A pure function of the source and the version: a second import gives identical bytes and dependencies.
			const Result<ImportResult> again = ImportSpriteSheet(fixture, "Assets/Tiles.spritesheet");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);
			CHECK(again->Dependencies == imported->Dependencies);

			// A sheet without a texture has no dependency.
			fixture.WriteProjectText("Assets/Plain.spritesheet", R"({"Format": "SpriteSheet", "Version": 1, "Frames": [{"Rect": {"Width": 4, "Height": 2}}]})");
			const Result<ImportResult> plain = ImportSpriteSheet(fixture, "Assets/Plain.spritesheet");
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			CHECK(plain->Dependencies.empty());
		}

		TEST_CASE("SpriteSheetImporter: an unknown member or an invalid sheet fails the import, located in its file" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Typo.spritesheet", R"({"Format": "SpriteSheet", "Version": 1, "Frame": []})");
			const Result<ImportResult> typo = ImportSpriteSheet(fixture, "Assets/Typo.spritesheet");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(typo.error().GetLocation().File.find("Typo.spritesheet") != std::string::npos);

			fixture.WriteProjectText("Assets/Bad.spritesheet",
				R"({"Format": "SpriteSheet", "Version": 1, "Frames": [], "Clips": [{"Name": "Run", "Frames": [{"Frame": 0, "Ticks": 1}]}]})");
			const Result<ImportResult> bad = ImportSpriteSheet(fixture, "Assets/Bad.spritesheet");
			REQUIRE_FALSE(bad.has_value());
			CHECK(bad.error().GetCode() == ErrorCode::ImportFailed);
			REQUIRE_FALSE(bad.error().GetIssues().empty());
			CHECK(bad.error().GetIssues().front().JsonPointer == "/Clips/0/Frames/0/Frame");

			fixture.WriteProjectText("Assets/Newer.spritesheet", R"({"Format": "SpriteSheet", "Version": 2})");
			const Result<ImportResult> newer = ImportSpriteSheet(fixture, "Assets/Newer.spritesheet");
			REQUIRE_FALSE(newer.has_value());
			CHECK(newer.error().GetCode() == ErrorCode::ImportFailed);
		}
	}

}
