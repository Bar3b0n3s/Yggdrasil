#include "TestsPCH.h"

#include "Engine/AssetPipeline/Importers/SceneImporter.h"

#include "Engine/Asset/DocumentData.h"
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

	}

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("SceneImporter: a version 0 scene cooks at the current version" * doctest::skip(true))
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
		}

		TEST_CASE("SceneImporter: a structurally invalid scene fails strictly with a located error" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			Result<std::string> invalid = Test::ReadTestDataText("Scenes/Invalid/DuplicateIds.scene");
			REQUIRE(invalid.has_value());
			fixture.WriteProjectText("Assets/Scenes/Broken.scene", *invalid);
			Result<ImportResult> imported = ImportScene(fixture, "Assets/Scenes/Broken.scene");
			REQUIRE_FALSE(imported.has_value());
			CHECK(imported.error().GetLocation().JsonPointer.has_value());
		}

		TEST_CASE("SceneImporter: the scene's asset references are its dependencies" * doctest::skip(true))
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
		}
	}

}
