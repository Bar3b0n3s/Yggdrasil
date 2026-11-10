#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/TilemapImporter.h"

#include "Engine/Asset/TilemapData.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <optional>
#include <string_view>
#include <vector>

// TilemapImporter (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 23, stream T1).

namespace Engine {

	namespace {

		// A 2 x 2-cell chunk size map over two sheets: chunk (0, 0) holds a ground row of entry 1 under a lone entry 2, chunk
		// (-1, 0) one ground cell.
		constexpr std::string_view GroundTilemap = R"({"Format": "Tilemap", "Version": 1, "CellSize": [1, 1], "ChunkSize": 2,
			"Palette": [{"Sheet": "5b5e000000000002", "Frame": 0, "Solid": true}, {"Sheet": "5b5e000000000003", "Frame": 4, "Solid": false}],
			"Chunks": [{"X": -1, "Y": 0, "Tiles": [0, 1, 0, 0]}, {"X": 0, "Y": 0, "Tiles": [1, 1, 2, 0]}]})";

		std::optional<ErrorCode> ErrorCodeOf(const Result<ImportResult>& result)
		{
			if (result.has_value())
				return std::nullopt;
			return result.error().GetCode();
		}

		Result<ImportResult> ImportTilemap(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x7113000000000001ull);
			metadata.Type = AssetType::Tilemap;
			metadata.Importer = std::string(TilemapImporter::Id);
			metadata.ImporterVersion = TilemapImporter::Version;
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
			Result<ImportResult> result = TilemapImporter().Import(context, metadata);
			// The importer reads nothing but its source: never a sheet.
			CHECK(context.GetDependencyReads().empty());
			return result;
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("TilemapImporter: a .tilemap imports strictly and lists its sheets as dependencies")
		{
			CHECK(TilemapImporter().CanImport(".tilemap"));
			CHECK(TilemapImporter().GetMainType() == AssetType::Tilemap);
			CHECK(TilemapImporter().GetSettingsTypeName().empty());
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Ground.tilemap", GroundTilemap);
			const Result<ImportResult> imported = ImportTilemap(fixture, "Assets/Ground.tilemap");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Tilemap);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x7113000000000001ull));
			// ADR 0028's phase-2 ledger: every new importer fills Import.Dependencies, here the palette's two sheets, sorted, though
			// neither is registered (that is the validator's ASSET_MISSING, never an import error).
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x5b5e000000000002ull), AssetHandle(0x5b5e000000000003ull) });
			CHECK(imported->Diagnostics.empty());
			const Result<AssetRef<TilemapData>> tilemap = LoadCookedTilemap(imported->Artifacts.front().Cooked);
			REQUIRE_MESSAGE(tilemap.has_value(), tilemap.error().ToString());
			CHECK((*tilemap)->ChunkSize == 2);
			REQUIRE((*tilemap)->Chunks.size() == 2);
			CHECK((*tilemap)->Chunks[0].X == -1);
			// Chunk (-1, 0) holds the cells x in [-2, -1], y in [0, 1], its Tiles from its bottom row up: (-1, 0) is its second.
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(-1, 0)) == 1);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(-2, 0)) == 0);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(0, 0)) == 1);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(1, 0)) == 1);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(0, 1)) == 2);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(1, 1)) == 0);
			CHECK(GetTilemapCell(**tilemap, glm::ivec2(5, 5)) == 0);

			// A pure function of the source and the version: a second import gives identical bytes.
			const Result<ImportResult> again = ImportTilemap(fixture, "Assets/Ground.tilemap");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == imported->Artifacts.front().Cooked);

			// Strict: an unknown member, an unsorted chunk list or a cell beyond the palette fails the import, located.
			fixture.WriteProjectText("Assets/Typo.tilemap", R"({"Format": "Tilemap", "Version": 1, "ChunkSize": 2, "Pallete": []})");
			const Result<ImportResult> typo = ImportTilemap(fixture, "Assets/Typo.tilemap");
			REQUIRE_FALSE(typo.has_value());
			CHECK(typo.error().GetCode() == ErrorCode::ImportFailed);
			fixture.WriteProjectText("Assets/Unsorted.tilemap", R"({"Format": "Tilemap", "Version": 1, "ChunkSize": 1,
				"Palette": [{"Sheet": null, "Frame": 0, "Solid": true}], "Chunks": [{"X": 1, "Y": 0, "Tiles": [1]}, {"X": 0, "Y": 0, "Tiles": [1]}]})");
			CHECK(ErrorCodeOf(ImportTilemap(fixture, "Assets/Unsorted.tilemap")) == ErrorCode::ImportFailed);
			fixture.WriteProjectText("Assets/Beyond.tilemap", R"({"Format": "Tilemap", "Version": 1, "ChunkSize": 1,
				"Palette": [{"Sheet": null, "Frame": 0, "Solid": true}], "Chunks": [{"X": 0, "Y": 0, "Tiles": [2]}]})");
			CHECK(ErrorCodeOf(ImportTilemap(fixture, "Assets/Beyond.tilemap")) == ErrorCode::ImportFailed);
		}
	}

}
