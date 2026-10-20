#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/PrefabImporter.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Scene/LoadReport.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Result<ImportResult> ImportPrefab(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x9fab000000000001ull);
			metadata.Type = AssetType::Prefab;
			metadata.Importer = std::string(PrefabImporter::Id);
			metadata.ImporterVersion = PrefabImporter::Version;
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
			return PrefabImporter().Import(context, metadata);
		}

		void CopyTestData(Test::AssetTestFixture& fixture, std::string_view source, std::string_view target)
		{
			Result<std::string> text = Test::ReadTestDataText(source);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			fixture.WriteProjectText(target, *text);
		}

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("PrefabImporter: cooks Block.prefab canonically, lists the prefabs it nests and rejects an instance on its root entity")
		{
			Test::AssetTestFixture fixture;
			CopyTestData(fixture, "Prefabs/Block.prefab", "Assets/Prefabs/Block.prefab");
			Result<ImportResult> imported = ImportPrefab(fixture, "Assets/Prefabs/Block.prefab");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			REQUIRE(imported->Artifacts.size() == 1);
			CHECK(imported->Artifacts.front().Type == AssetType::Prefab);
			CHECK(imported->Artifacts.front().Handle == AssetHandle(0x9fab000000000001ull));
			Result<AssetRef<PrefabData>> prefab = LoadCookedPrefab(imported->Artifacts.front().Cooked);
			REQUIRE(prefab.has_value());
			Json document = *(*prefab)->Document;
			CHECK(document["Format"] == Json("Prefab"));
			CHECK(document.contains("Root"));
			// The cooked document is the canonical file itself.
			Result<std::string> canonical = JsonWriter::Write(document);
			REQUIRE(canonical.has_value());
			CHECK(*canonical == AsStringView(fixture.ReadProjectFile("Assets/Prefabs/Block.prefab")));
			CHECK(imported->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x101) });

			// Live nested prefabs (Docs/Decisions/0031-m20-contract.md decision 8): an instance below the root imports, and the
			// prefab it nests is a dependency (M19's rule that every importer fills Import.Dependencies), which the editor's
			// nesting graph reads.
			Result<Json> nested = JsonReader::Parse(AsStringView(fixture.ReadProjectFile("Assets/Prefabs/Block.prefab")));
			REQUIRE(nested.has_value());
			const Json member = (*nested)["Entities"][1];
			Json instance = Json::object();
			instance["Prefab"] = "3c9f2e7a11d04b88";
			instance["Overrides"] = Json::array();
			Json link = Json::object();
			link["PrefabEntityID"] = "00000000000e0001";
			link["InstanceRoot"] = member["ID"];
			link["OuterPrefabEntityID"] = nullptr;
			(*nested)["Entities"][1]["Components"]["Prefab"] = instance;
			(*nested)["Entities"][1]["Components"]["PrefabLink"] = link;
			Result<std::string> nestedText = JsonWriter::Write(*nested);
			REQUIRE(nestedText.has_value());
			fixture.WriteProjectText("Assets/Prefabs/Nested.prefab", *nestedText);
			Result<ImportResult> nesting = ImportPrefab(fixture, "Assets/Prefabs/Nested.prefab");
			REQUIRE_MESSAGE(nesting.has_value(), nesting.error().ToString());
			CHECK(nesting->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x101), AssetHandle(0x3c9f2e7a11d04b88ull) });

			// The prefab's root is never an instance root itself: a Prefab component there fails the strict load.
			Result<Json> rooted = JsonReader::Parse(AsStringView(fixture.ReadProjectFile("Assets/Prefabs/Block.prefab")));
			REQUIRE(rooted.has_value());
			(*rooted)["Entities"][0]["Components"]["Prefab"] = instance;
			Result<std::string> rootedText = JsonWriter::Write(*rooted);
			REQUIRE(rootedText.has_value());
			fixture.WriteProjectText("Assets/Prefabs/Rooted.prefab", *rootedText);
			Result<ImportResult> rejected = ImportPrefab(fixture, "Assets/Prefabs/Rooted.prefab");
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(rejected.error().ToString().find(std::string(PrefabNestedInstanceCode)) != std::string::npos);
		}

		TEST_CASE("PrefabImporter: a prefab without a valid root fails and its asset references are its dependencies")
		{
			Test::AssetTestFixture fixture;
			CopyTestData(fixture, "Assets/Prefabs/Lamp.prefab", "Assets/Prefabs/Lamp.prefab");
			Result<ImportResult> lamp = ImportPrefab(fixture, "Assets/Prefabs/Lamp.prefab");
			REQUIRE_MESSAGE(lamp.has_value(), lamp.error().ToString());
			CHECK(lamp->Dependencies == std::vector<AssetHandle>{ AssetHandle(0x102), AssetHandle(0xa41f0c2290b1d3e4ull) });
			Result<ImportResult> again = ImportPrefab(fixture, "Assets/Prefabs/Lamp.prefab");
			REQUIRE(again.has_value());
			CHECK(again->Artifacts.front().Cooked == lamp->Artifacts.front().Cooked);

			Result<Json> document = JsonReader::Parse(AsStringView(fixture.ReadProjectFile("Assets/Prefabs/Lamp.prefab")));
			REQUIRE(document.has_value());
			(*document)["Root"] = "00000000000c0009";
			Result<std::string> text = JsonWriter::Write(*document);
			REQUIRE(text.has_value());
			fixture.WriteProjectText("Assets/Prefabs/Rootless.prefab", *text);
			Result<ImportResult> rootless = ImportPrefab(fixture, "Assets/Prefabs/Rootless.prefab");
			REQUIRE_FALSE(rootless.has_value());
			CHECK(rootless.error().ToString().find(std::string(PrefabInvalidRootCode)) != std::string::npos);
		}
	}

}
