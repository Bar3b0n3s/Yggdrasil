#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/SceneImporter.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Result<ImportResult> ImportScene(Test::AssetTestFixture& fixture, std::string_view relative)
		{
			const Buffer bytes = fixture.ReadProjectFile(relative);
			AssetMetadata metadata;
			metadata.Handle = AssetHandle(0x5ce0000000000001ull);
			metadata.Type = AssetType::Scene;
			metadata.Importer = std::string(SceneImporter::Id);
			metadata.ImporterVersion = SceneImporter::Version;
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
			return SceneImporter().Import(context, metadata);
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
		TEST_CASE("SceneImporter: a version 0 scene cooks at the current version")
		{
			Test::AssetTestFixture fixture;
			Result<std::string> v0 = Test::ReadTestDataText("Formats/Scene/v0.scene");
			REQUIRE(v0.has_value());
			fixture.WriteProjectText("Assets/Scenes/Old.scene", *v0);
			Result<ImportResult> imported = ImportScene(fixture, "Assets/Scenes/Old.scene");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			Result<AssetRef<SceneData>> scene = LoadCookedScene(imported->Artifacts.front().Cooked);
			REQUIRE(scene.has_value());
			Json document = *(*scene)->Document;
			CHECK(document["Version"] == Json(1));
			CHECK(document["Format"] == Json("Scene"));

			// The cooked document is the canonical save of the upgraded scene.
			Result<std::string> upgraded = Test::ReadTestDataText("Formats/Scene/v0.upgraded.scene");
			REQUIRE(upgraded.has_value());
			Result<std::string> written = JsonWriter::Write(document);
			REQUIRE(written.has_value());
			CHECK(*written == *upgraded);
		}

		TEST_CASE("SceneImporter: a structurally invalid scene fails strictly with a located error")
		{
			Test::AssetTestFixture fixture;
			Result<std::string> invalid = Test::ReadTestDataText("Scenes/Invalid/DuplicateIds.scene");
			REQUIRE(invalid.has_value());
			fixture.WriteProjectText("Assets/Scenes/Broken.scene", *invalid);
			Result<ImportResult> imported = ImportScene(fixture, "Assets/Scenes/Broken.scene");
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetLocation().JsonPointer.has_value());

			fixture.WriteProjectText("Assets/Scenes/Text.scene", "not json");
			Result<ImportResult> text = ImportScene(fixture, "Assets/Scenes/Text.scene");
			REQUIRE_FALSE(text.has_value());
			CHECK(text.error().GetCode() == ErrorCode::Parse);

			Result<std::string> newer = Test::ReadTestDataText("Formats/Scene/NewerVersion.scene");
			REQUIRE(newer.has_value());
			fixture.WriteProjectText("Assets/Scenes/Newer.scene", *newer);
			Result<ImportResult> future = ImportScene(fixture, "Assets/Scenes/Newer.scene");
			REQUIRE_FALSE(future.has_value());
			CHECK(future.error().GetCode() == ErrorCode::UnsupportedVersion);
		}

		TEST_CASE("SceneImporter: the scene's asset references are its dependencies")
		{
			Test::AssetTestFixture fixture;
			Result<std::string> scene = Test::ReadTestDataText("Scenes/AllComponents.scene");
			REQUIRE(scene.has_value());
			fixture.WriteProjectText("Assets/Scenes/All.scene", *scene);
			Result<ImportResult> imported = ImportScene(fixture, "Assets/Scenes/All.scene");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			CHECK_FALSE(imported->Dependencies.empty());
			CHECK(std::ranges::is_sorted(imported->Dependencies));
			CHECK(std::ranges::find(imported->Dependencies, AssetHandle(0x3c9f2e7a11d04b88ull)) != imported->Dependencies.end());
			// Every AssetRef field of the fixture, at any depth, and never a null slot or an entity reference.
			const std::vector<AssetHandle> expected = { AssetHandle(0x101), AssetHandle(0x102), AssetHandle(0x103), AssetHandle(0x201),
				AssetHandle(0x301), AssetHandle(0x401), AssetHandle(0x3c9f2e7a11d04b88ull), AssetHandle(0xa41f0c2290b1d3e4ull),
				AssetHandle(0xc0ffee0000000001ull) };
			CHECK(imported->Dependencies == expected);
		}

		TEST_CASE("SceneImporter: an override whose value is an asset reference is a dependency")
		{
			Test::AssetTestFixture fixture;
			CopyTestData(fixture, "Assets/Scenes/Level.scene", "Assets/Scenes/Level.scene");
			Result<ImportResult> imported = ImportScene(fixture, "Assets/Scenes/Level.scene");
			REQUIRE_MESSAGE(imported.has_value(), imported.error().ToString());
			CHECK(imported->Dependencies
				== std::vector<AssetHandle>{ AssetHandle(0x103), AssetHandle(0x107), AssetHandle(0x3a7e000000000002ull), AssetHandle(0x5b1e7c40d29a8f13ull),
					AssetHandle(0xa41f0c2290b1d3e4ull) });

			// The overridden mesh appears nowhere but in the override's Value: it is found through the Variant's schema.
			Result<std::string> text = Test::ReadTestDataText("Assets/Scenes/Level.scene");
			REQUIRE(text.has_value());
			Result<Json> document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			(*document)["Entities"][3]["Components"]["Prefab"]["Overrides"][0]["Value"] = "0000000000000106";
			Result<std::string> edited = JsonWriter::Write(*document);
			REQUIRE(edited.has_value());
			fixture.WriteProjectText("Assets/Scenes/Override.scene", *edited);
			Result<ImportResult> overridden = ImportScene(fixture, "Assets/Scenes/Override.scene");
			REQUIRE_MESSAGE(overridden.has_value(), overridden.error().ToString());
			CHECK(std::ranges::find(overridden->Dependencies, AssetHandle(0x106)) != overridden->Dependencies.end());
		}

		TEST_CASE("SceneImporter: importing twice gives identical bytes")
		{
			Test::AssetTestFixture fixture;
			CopyTestData(fixture, "Assets/Scenes/Level.scene", "Assets/Scenes/Level.scene");
			CopyTestData(fixture, "Scenes/AllComponents.scene", "Assets/Scenes/All.scene");
			for (const std::string_view path : { "Assets/Scenes/Level.scene", "Assets/Scenes/All.scene" })
			{
				CAPTURE(std::string(path));
				Result<ImportResult> first = ImportScene(fixture, path);
				Result<ImportResult> second = ImportScene(fixture, path);
				REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
				REQUIRE(second.has_value());
				CHECK(first->Artifacts.front().Cooked == second->Artifacts.front().Cooked);
				CHECK(first->Dependencies == second->Dependencies);
			}
		}

		TEST_CASE("SceneImporter: runs on the main thread")
		{
			CHECK(SceneImporter().RequiresMainThread());
			CHECK(SceneImporter().GetSettingsTypeName().empty());
			CHECK(SceneImporter().GetMainType() == AssetType::Scene);
		}
	}

}
