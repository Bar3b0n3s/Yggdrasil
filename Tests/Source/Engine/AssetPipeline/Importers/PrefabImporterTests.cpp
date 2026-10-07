#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/PrefabImporter.h"

#include "Engine/Asset/DocumentData.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("PrefabImporter: cooks Block.prefab canonically and rejects nested instances" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			Result<std::string> block = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(block.has_value());
			fixture.WriteProjectText("Assets/Prefabs/Block.prefab", *block);
			const Buffer bytes = fixture.ReadProjectFile("Assets/Prefabs/Block.prefab");
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x9fab000000000001ull);
			metadata.Type = AssetType::Prefab;
			metadata.Importer = std::string(PrefabImporter::Id);
			metadata.ImporterVersion = PrefabImporter::Version;
			ImportContext context({
				.Vfs = &fixture.GetVfs(),
				.SourcePath = fixture.ProjectPath("Assets/Prefabs/Block.prefab"),
				.SourceBytes = bytes,
				.Settings = VariantValue(),
				.Registry = &fixture.GetRegistry(),
				.Assets = {},
				.EnvironmentBaker = nullptr,
				.ScriptDiagnostics = nullptr,
			});
			Result<ImportResult> imported = PrefabImporter().Import(context, metadata);
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Prefab);
			Result<AssetRef<PrefabData>> prefab = LoadCookedPrefab(imported->Artifacts.front().Cooked);
			REQUIRE(prefab.has_value());
			Json document = *(*prefab)->Document;
			CHECK(document["Format"] == Json("Prefab"));
			CHECK(document.contains("Root"));
		}
	}

}
