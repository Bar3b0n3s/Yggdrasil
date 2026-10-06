#include "TestsPCH.h"

#include "Engine/Scene/PrefabInstantiator.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/GlmApprox.h"
#include "Support/SceneTestFixture.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

namespace Engine {

	// The Block prefab: a root with one child "Visual" carrying a MeshRenderer (Tests/Data/Prefabs/Block.prefab).
	static Prefab LoadBlockPrefab(const TypeRegistry& registry)
	{
		const Result<std::string> text = Test::ReadTestDataText("Prefabs/Block.prefab");
		REQUIRE(text.has_value());
		LoadReport report;
		Result<Prefab> prefab = Prefab::LoadFromString(*text, registry, LoadOptions{}, report);
		REQUIRE(prefab.has_value());
		return std::move(*prefab);
	}

	// `prefab` with the child's mesh replaced and a new child "Light" added (an edited asset).
	static Prefab EditBlockPrefab(const TypeRegistry& registry, const Prefab& prefab)
	{
		Result<Json> document = prefab.ToJson();
		REQUIRE(document.has_value());
		Json& visual = (*document)["Entities"][1];
		visual["Components"]["MeshRenderer"]["Mesh"] = "0000000000000105";
		visual["Components"]["MeshRenderer"]["CastShadows"] = false;
		Json light = (*document)["Entities"][0];
		light["ID"] = "00000000000b0003";
		light["Name"] = "Light";
		light["Parent"] = "00000000000b0001";
		(*document)["Entities"].push_back(light);
		LoadReport report;
		Result<Prefab> edited = Prefab::FromJson(*document, registry, LoadOptions{}, report);
		REQUIRE(edited.has_value());
		return std::move(*edited);
	}

	static PrefabInstantiateOptions MakeOptions(UUID rootID)
	{
		PrefabInstantiateOptions options;
		options.PrefabHandle = AssetHandle(0x3c9f2e7a11d04b88);
		options.RootID = rootID;
		return options;
	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Prefab: instance IDs are Hash64(root, prefabEntity)" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			const UUID rootID(0x6b2d4f8a10c3e507);
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());

			CHECK(root->GetUUID() == rootID);
			CHECK(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002)) == UUID(Hash64(rootID.GetValue(), 0x00000000000b0002)));
			CHECK(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002)) == UUID(0xec685b183765f2e5)); // the AllComponents fixture
			REQUIRE(root->GetChildren().size() == 1);
			CHECK(root->GetChildren()[0] == PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002)));

			REQUIRE(root->HasComponent<PrefabInstanceComponent>());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Prefab.GetHandle() == AssetHandle(0x3c9f2e7a11d04b88));
			CHECK(root->GetComponent<PrefabLinkComponent>().PrefabEntityID == UUID(0x00000000000b0001));
			const Entity visual = setup.GetScene().FindEntityByID(root->GetChildren()[0]);
			REQUIRE(visual.IsValid());
			CHECK(visual.GetComponent<PrefabLinkComponent>().InstanceRoot == rootID);
			CHECK_FALSE(visual.HasComponent<PrefabInstanceComponent>());

			// The same root ID in another scene derives the same member IDs.
			const Scope<Scene> other = setup.CreateEmptyScene();
			const Result<Entity> again = PrefabInstantiator::Instantiate(*other, prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(again.has_value());
			CHECK(again->GetChildren()[0] == root->GetChildren()[0]);
		}

		TEST_CASE("Prefab: field override survives a prefab edit" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x1111)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity visual = setup.GetScene().FindEntityByID(root->GetChildren()[0]);
			visual.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 2.0f, 1.0f);
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<PrefabOverride>& overrides = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(overrides.size() == 1);
			CHECK(overrides[0].PrefabEntityID == UUID(0x00000000000b0002));
			CHECK(overrides[0].Kind == PrefabOverrideKind::Field);
			CHECK(overrides[0].Component == "Transform");
			CHECK(overrides[0].Field == "Scale");

			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, edited, PrefabOptions{}, report).has_value());

			const Entity updated = setup.GetScene().FindEntityByID(PrefabInstantiator::DeriveInstanceID(UUID(0x1111), UUID(0x00000000000b0002)));
			REQUIRE(updated.IsValid());
			CHECK(updated.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f, 2.0f, 1.0f));                   // the override
			CHECK(updated.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == AssetHandle(0x0000000000000105)); // the edit
			CHECK_FALSE(updated.GetComponent<MeshRendererComponent>().CastShadows);
			CHECK(root->GetChildren().size() == 2); // the new prefab entity appeared
		}

		TEST_CASE("Prefab: external EntityRef into an instance survives update" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x2222)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID target = root->GetChildren()[0];

			// An entity outside the instance references the member through a script field (an Entity-kind Variant).
			const Entity watcher = setup.GetScene().CreateEntity("Watcher");
			ScriptComponent& script = watcher.AddComponent<ScriptComponent>();
			script.Fields["Target"] = VariantValue(Json(target.ToString()));

			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, edited, PrefabOptions{}, report).has_value());
			const Entity resolved = setup.GetScene().FindEntityByID(target);
			REQUIRE(resolved.IsValid());
			CHECK(resolved.GetName() == "Visual");
			CHECK(watcher.GetComponent<ScriptComponent>().Fields.at("Target").Get() == Json(target.ToString()));
		}

		TEST_CASE("Prefab: user-added children are preserved" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x3333)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity visual = setup.GetScene().FindEntityByID(root->GetChildren()[0]);
			const Entity userChild = setup.GetScene().CreateEntity("UserChild", visual);
			const Entity userRootChild = setup.GetScene().CreateEntity("UserRootChild", *root);

			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, edited, PrefabOptions{}, report).has_value());
			REQUIRE(userChild.IsValid());
			REQUIRE(userRootChild.IsValid());
			CHECK_FALSE(userChild.HasComponent<PrefabLinkComponent>());
			CHECK(userChild.GetParent().GetUUID() == visual.GetUUID());
			CHECK(userRootChild.GetParent().GetUUID() == root->GetUUID());

			// Overrides do not record user children.
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, prefab, PrefabOptions{});
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());
		}

		TEST_CASE("PrefabInstantiator: internal references are remapped to the derived IDs" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			Result<Json> document = JsonReader::Parse(R"({
				"Format": "Prefab", "Version": 1, "Name": "Pair", "Root": "00000000000c0001",
				"ComponentVersions": { "Transform": 1 },
				"Entities": [
					{ "ID": "00000000000c0001", "Name": "Root", "Parent": null, "Active": true, "Tags": [],
					  "Components": { "Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] } } },
					{ "ID": "00000000000c0002", "Name": "Child", "Parent": "00000000000c0001", "Active": true, "Tags": [],
					  "Components": { "Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] } } }
				]
			})");
			REQUIRE(document.has_value());
			LoadReport report;
			const Result<Prefab> prefab = Prefab::FromJson(*document, setup.GetRegistry(), LoadOptions{}, report);
			REQUIRE(prefab.has_value());
			const Result<Entity> root =
				PrefabInstantiator::Instantiate(setup.GetScene(), *prefab, MakeOptions(UUID(0x4444)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity child = setup.GetScene().FindEntityByID(PrefabInstantiator::DeriveInstanceID(UUID(0x4444), UUID(0x00000000000c0002)));
			REQUIRE(child.IsValid());
			CHECK(child.GetParent() == *root);

			// The remapped parent link is the instance-space image of the prefab's, so it is not an override.
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, *prefab, PrefabOptions{});
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());
		}

		TEST_CASE("Prefab: a deactivated member stays inactive across updates" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const UUID rootID(0x9999);
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002));
			setup.GetScene().FindEntityByID(visualID).SetActive(false);

			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<PrefabOverride> overrides = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(overrides.size() == 1);
			CHECK(overrides[0].PrefabEntityID == UUID(0x00000000000b0002));
			CHECK(overrides[0].Kind == PrefabOverrideKind::EntityKey);
			CHECK(overrides[0].Component.empty());
			CHECK(overrides[0].Field == "Active");
			CHECK(overrides[0].Value.Get() == Json(false));

			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			const Status updated = PrefabInstantiator::UpdateInstance(setup.GetScene(), setup.GetScene().FindEntityByID(rootID), edited,
				PrefabOptions{}, report);
			REQUIRE(updated.has_value());
			const Entity visual = setup.GetScene().FindEntityByID(visualID);
			REQUIRE(visual.IsValid());
			CHECK_FALSE(visual.IsActiveSelf());

			const Status reverted = PrefabInstantiator::Revert(setup.GetScene(), setup.GetScene().FindEntityByID(rootID), edited,
				PrefabOptions{}, report);
			REQUIRE(reverted.has_value());
			CHECK(setup.GetScene().FindEntityByID(visualID).IsActiveSelf());
		}

		TEST_CASE("PrefabInstantiator: Entity-kind script fields inside the prefab follow the derived IDs" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;

			// The root's script field "Goal" (an EntityRef in the fixture schema) names the prefab-local ID of the barrel.
			const Result<Json> document = JsonReader::Parse(R"({
				"Format": "Prefab", "Version": 1, "Name": "Turret", "Root": "00000000000d0001",
				"ComponentVersions": { "Transform": 1, "Script": 1 },
				"Entities": [
					{ "ID": "00000000000d0001", "Name": "Turret", "Parent": null, "Active": true, "Tags": [],
					  "Components": {
						"Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] },
						"Script": { "Script": "00000000c0ffee01", "Fields": { "Goal": "00000000000d0002" }, "ExecutionOrder": 0 } } },
					{ "ID": "00000000000d0002", "Name": "Barrel", "Parent": "00000000000d0001", "Active": true, "Tags": [],
					  "Components": { "Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] } } }
				]
			})");
			REQUIRE(document.has_value());
			LoadOptions loadOptions;
			loadOptions.Schemas = &schemas;
			LoadReport report;
			const Result<Prefab> prefab = Prefab::FromJson(*document, setup.GetRegistry(), loadOptions, report);
			REQUIRE(prefab.has_value());

			const UUID rootID(0x8888);
			const Json barrel(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000d0002)).ToString());
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), *prefab, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());
			CHECK(root->GetComponent<ScriptComponent>().Fields.at("Goal").Get() == barrel);

			// A fresh instance has no overrides: the prefab side is remapped before the comparison.
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, *prefab, options);
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());

			// Rebuilding the instance keeps the reference on the derived member.
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, *prefab, options, report).has_value());
			const Entity rebuilt = setup.GetScene().FindEntityByID(rootID);
			REQUIRE(rebuilt.IsValid());
			CHECK(rebuilt.GetComponent<ScriptComponent>().Fields.at("Goal").Get() == barrel);
		}

		TEST_CASE("PrefabInstantiator: unpack removes the links and keeps the entities" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x5555)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			REQUIRE(PrefabInstantiator::Unpack(*root).has_value());
			CHECK_FALSE(root->HasComponent<PrefabInstanceComponent>());
			CHECK_FALSE(root->HasComponent<PrefabLinkComponent>());
			CHECK(setup.GetScene().GetEntityCount() == 2);
			CHECK_FALSE(PrefabInstantiator::Unpack(*root).has_value()); // no longer an instance root
		}

		TEST_CASE("PrefabInstantiator: a used root ID or an empty prefab is rejected" * doctest::skip(true))
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			static_cast<void>(setup.GetScene().CreateEntityWithID(UUID(0x6666), "Taken"));
			LoadReport report;
			CHECK_FALSE(PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID()), PrefabOptions{}, report).has_value());
			CHECK_FALSE(PrefabInstantiator::Instantiate(setup.GetScene(), Prefab(), MakeOptions(UUID(0x7777)), PrefabOptions{}, report).has_value());
			const Result<Entity> taken =
				PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x6666)), PrefabOptions{}, report);
			REQUIRE_FALSE(taken.has_value());
			CHECK(setup.GetScene().GetEntityCount() == 1);
		}
	}

}
