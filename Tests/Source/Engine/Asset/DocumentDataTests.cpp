#include "TestsPCH.h"

#include "Engine/Asset/DocumentData.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		Json ParseDocument(std::string_view text)
		{
			Result<Json> document = JsonReader::Parse(text);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			return std::move(*document);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("DocumentData: scene and prefab documents cook and load" * doctest::skip(true))
		{
			Result<std::string> sceneText = Test::ReadTestDataText("Scenes/AllComponents.scene");
			REQUIRE(sceneText.has_value());
			const Json scene = ParseDocument(*sceneText);
			Result<Buffer> cookedScene = CookDocument(AssetType::Scene, scene, 1);
			REQUIRE_MESSAGE(cookedScene.has_value(), cookedScene.error().ToString());
			Result<AssetRef<SceneData>> loadedScene = LoadCookedScene(*cookedScene);
			REQUIRE_MESSAGE(loadedScene.has_value(), loadedScene.error().ToString());
			REQUIRE((*loadedScene)->Document != nullptr);
			CHECK(*(*loadedScene)->Document == scene);
			// Identical documents give identical bytes.
			CHECK(CookDocument(AssetType::Scene, scene, 1).value_or(Buffer()) == *cookedScene);

			Result<std::string> prefabText = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(prefabText.has_value());
			const Json prefab = ParseDocument(*prefabText);
			Result<Buffer> cookedPrefab = CookDocument(AssetType::Prefab, prefab, 1);
			REQUIRE(cookedPrefab.has_value());
			Result<AssetRef<PrefabData>> loadedPrefab = LoadCookedPrefab(*cookedPrefab);
			REQUIRE(loadedPrefab.has_value());
			CHECK(*(*loadedPrefab)->Document == prefab);
		}

		TEST_CASE("DocumentData: a document of another format or type is rejected" * doctest::skip(true))
		{
			const Json scene = ParseDocument(R"({"Format": "Scene", "Version": 1, "Name": "Empty", "Seed": 0, "ComponentVersions": {}, "Entities": []})");
			Result<Buffer> cooked = CookDocument(AssetType::Scene, scene, 1);
			REQUIRE(cooked.has_value());
			Result<AssetRef<PrefabData>> asPrefab = LoadCookedPrefab(*cooked);
			REQUIRE_FALSE(asPrefab.has_value());
			CHECK(asPrefab.error().GetCode() == ErrorCode::Validation);

			const Json wrongFormat = ParseDocument(R"({"Format": "Prefab", "Version": 1})");
			Result<Buffer> mislabelled = CookDocument(AssetType::Scene, wrongFormat, 1);
			REQUIRE(mislabelled.has_value());
			CHECK_FALSE(LoadCookedScene(*mislabelled).has_value());
		}
	}

}
