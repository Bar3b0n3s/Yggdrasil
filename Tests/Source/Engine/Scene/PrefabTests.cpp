#include "TestsPCH.h"

#include "Engine/Scene/Prefab.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Entity.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("Prefab: the Block fixture loads and re-saves byte-identically" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> text = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(text.has_value());

			LoadReport report;
			const Result<Prefab> prefab = Prefab::LoadFromString(*text, *registry, LoadOptions{}, report);
			REQUIRE(prefab.has_value());
			CHECK(prefab->GetName() == "Block");
			CHECK(prefab->GetRootID() == UUID(0x00000000000b0001));
			CHECK(prefab->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000b0001), UUID(0x00000000000b0002) });
			REQUIRE(prefab->FindEntity(UUID(0x00000000000b0002)) != nullptr);
			CHECK((*prefab->FindEntity(UUID(0x00000000000b0002)))["Name"] == "Visual");

			const Result<std::string> saved = prefab->SaveToString();
			REQUIRE(saved.has_value());
			CHECK(*saved == *text);
		}

		TEST_CASE("Prefab: creating a prefab flattens nested instances and nulls the root's parent" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Entity holder = scene.CreateEntity("Holder");
			const Entity root = scene.CreateEntity("Root", holder);
			const Entity nested = scene.CreateEntity("Nested", root);
			nested.AddComponent<PrefabInstanceComponent>();
			nested.AddComponent<PrefabLinkComponent>(PrefabLinkComponent{ UUID(0x77), nested.GetUUID() });
			root.AddComponent<MeshRendererComponent>();

			const Result<Prefab> prefab = Prefab::CreateFromEntity(root, "Thing");
			REQUIRE(prefab.has_value());
			CHECK(prefab->GetRootID() == root.GetUUID());
			CHECK(prefab->GetEntityIDs().size() == 2);
			const Json* rootJson = prefab->FindEntity(root.GetUUID());
			REQUIRE(rootJson != nullptr);
			CHECK((*rootJson)["Parent"].is_null());
			const Json* nestedJson = prefab->FindEntity(nested.GetUUID());
			REQUIRE(nestedJson != nullptr);
			CHECK_FALSE((*nestedJson)["Components"].contains("Prefab"));
			CHECK_FALSE((*nestedJson)["Components"].contains("PrefabLink"));
		}

		TEST_CASE("Prefab: documents with prefab links or several roots are rejected" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			LoadReport report;
			const Result<Prefab> twoRoots = Prefab::LoadFromString(R"({
				"Format": "Prefab", "Version": 1, "Name": "Bad", "Root": "00000000000b0001", "ComponentVersions": {},
				"Entities": [ { "ID": "00000000000b0001", "Name": "A", "Parent": null, "Active": true, "Tags": [], "Components": {} },
				              { "ID": "00000000000b0002", "Name": "B", "Parent": null, "Active": true, "Tags": [], "Components": {} } ]
			})",
				*registry, LoadOptions{}, report);
			REQUIRE_FALSE(twoRoots.has_value());
			CHECK(twoRoots.error().GetCode() == ErrorCode::Validation);

			const Result<Prefab> sceneFormat = Prefab::LoadFromString(R"({ "Format": "Scene", "Version": 1 })", *registry, LoadOptions{}, report);
			CHECK_FALSE(sceneFormat.has_value());
		}
	}

}
