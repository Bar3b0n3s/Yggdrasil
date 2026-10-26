#include "TestsPCH.h"

#include "Engine/Scene/PrefabInstantiator.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/EnvironmentComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/TransformSystem.h"
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

	// The prefab "Shelf" (Docs/Decisions/0031-m20-contract.md decision 8): a root (shelf-local id 0x7979) holding a live Block
	// instance at the shelf-local id 0x7a7a, whose record in the shelf's document is one override of the block (its Visual
	// receives no shadows). Built in a scratch scene of `setup`, as prefab.create builds a prefab from entities.
	static Prefab MakeShelfPrefab(Test::SceneTestFixture& setup, const Prefab& block)
	{
		const Scope<Scene> source = setup.CreateEmptyScene();
		const Entity shelfRoot = source->CreateEntityWithID(UUID(0x7979), "Shelf");
		PrefabInstantiateOptions nestedOptions;
		nestedOptions.PrefabHandle = AssetHandle(0x3c9f2e7a11d04b88);
		nestedOptions.RootID = UUID(0x7a7a);
		nestedOptions.Parent = shelfRoot;
		LoadReport report;
		const Result<Entity> nested = PrefabInstantiator::Instantiate(*source, block, nestedOptions, PrefabOptions{}, report);
		REQUIRE(nested.has_value());
		source->FindEntityByID(PrefabInstantiator::DeriveInstanceID(UUID(0x7a7a), UUID(0x00000000000b0002))).Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
		{
			renderer.ReceiveShadows = false;
		});
		REQUIRE(PrefabInstantiator::RefreshOverrides(*nested, block, PrefabOptions{}).has_value());
		Result<Prefab> shelf = Prefab::CreateFromEntity(shelfRoot, "Shelf");
		REQUIRE_MESSAGE(shelf.has_value(), (shelf ? std::string() : shelf.error().ToString()));
		return std::move(*shelf);
	}

	// The Prefab component of the entity `id` of `prefab`'s document.
	static Json GetDocumentInstance(const Prefab& prefab, UUID id)
	{
		const Json* entity = prefab.FindEntity(id);
		REQUIRE(entity != nullptr);
		REQUIRE((*entity)["Components"].contains("Prefab"));
		return (*entity)["Components"]["Prefab"];
	}

	static PrefabInstantiateOptions MakeOptions(UUID rootID)
	{
		PrefabInstantiateOptions options;
		options.PrefabHandle = AssetHandle(0x3c9f2e7a11d04b88);
		options.RootID = rootID;
		return options;
	}

	// A prefab read from `text` (a prefab document), its script fields resolved through `schemas` when given.
	static Prefab ParsePrefab(const TypeRegistry& registry, std::string_view text, const IFieldSchemaSource* schemas = nullptr)
	{
		LoadOptions options;
		options.Schemas = schemas;
		LoadReport report;
		Result<Prefab> prefab = Prefab::LoadFromString(text, registry, options, report);
		INFO((prefab.has_value() ? std::string() : prefab.error().ToString()));
		REQUIRE(prefab.has_value());
		return std::move(*prefab);
	}

	static PrefabOverride MakeOverride(UUID prefabEntity, PrefabOverrideKind kind, std::string component, std::string field, Json value)
	{
		PrefabOverride prefabOverride;
		prefabOverride.Path = { prefabEntity };
		prefabOverride.Kind = kind;
		prefabOverride.Component = std::move(component);
		prefabOverride.Field = std::move(field);
		prefabOverride.Value = VariantValue(std::move(value));
		return prefabOverride;
	}

	static size_t CountDiagnostics(const LoadReport& report, std::string_view code)
	{
		return static_cast<size_t>(std::count_if(report.Diagnostics.begin(), report.Diagnostics.end(), [code](const LoadDiagnostic& diagnostic)
		{
			return diagnostic.Code == code;
		}));
	}

	// The error code of `result`, which must have failed.
	template<typename T>
	static ErrorCode GetErrorCode(const Result<T>& result)
	{
		REQUIRE_FALSE(result.has_value());
		return result.error().GetCode();
	}

	// The change `changes` records for `id`, or nullptr.
	static const EntityChange* FindChange(const std::vector<EntityChange>& changes, UUID id)
	{
		const auto found = std::find_if(changes.begin(), changes.end(), [id](const EntityChange& change)
		{
			return change.EntityID == id;
		});
		return found != changes.end() ? &*found : nullptr;
	}

	// Root -> A -> B, each with a Transform (missing fields read as their defaults); A and B each sit one metre above their
	// parent.
	constexpr std::string_view ChainPrefab = R"({
		"Format": "Prefab", "Version": 1, "Name": "Chain", "Root": "00000000000f0001", "ComponentVersions": { "Transform": 1 },
		"Entities": [
			{ "ID": "00000000000f0001", "Name": "Root", "Parent": null, "Active": true, "Tags": [], "Components": { "Transform": {} } },
			{ "ID": "00000000000f0002", "Name": "A", "Parent": "00000000000f0001", "Active": true, "Tags": [],
			  "Components": { "Transform": { "Translation": [0, 1, 0] } } },
			{ "ID": "00000000000f0003", "Name": "B", "Parent": "00000000000f0002", "Active": true, "Tags": [],
			  "Components": { "Transform": { "Translation": [0, 1, 0] } } }
		]
	})";

	// `prefab` without the entities at `indices` of its document (in descending order, so earlier indices stay valid).
	static Prefab RemovePrefabEntities(const TypeRegistry& registry, const Prefab& prefab, std::initializer_list<size_t> indices)
	{
		Result<Json> document = prefab.ToJson();
		REQUIRE(document.has_value());
		for (const size_t index : indices)
			(*document)["Entities"].erase(index);
		LoadReport report;
		Result<Prefab> edited = Prefab::FromJson(*document, registry, LoadOptions{}, report);
		REQUIRE(edited.has_value());
		return std::move(*edited);
	}

	// A turret whose script field "Goal" (an EntityRef in the fixture schema) names the prefab-local ID of its barrel.
	constexpr std::string_view TurretPrefab = R"({
		"Format": "Prefab", "Version": 1, "Name": "Turret", "Root": "00000000000d0001",
		"ComponentVersions": { "Transform": 1, "Script": 2 },
		"Entities": [
			{ "ID": "00000000000d0001", "Name": "Turret", "Parent": null, "Active": true, "Tags": [],
			  "Components": {
				"Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] },
				"Script": { "Scripts": [ { "Script": "00000000c0ffee01", "Fields": { "Goal": "00000000000d0002" }, "ExecutionOrder": 0 } ] } } },
			{ "ID": "00000000000d0002", "Name": "Barrel", "Parent": "00000000000d0001", "Active": true, "Tags": [],
			  "Components": { "Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] } } }
		]
	})";

	// A turret with two behaviours in its Script list (Script version 2, Docs/Decisions/0031-m20-contract.md decision 17):
	// slot 0 aims (Torque), slot 1 fires (Count).
	constexpr std::string_view TwoSlotTurretPrefab = R"({
		"Format": "Prefab", "Version": 1, "Name": "Turret", "Root": "00000000000f0001",
		"ComponentVersions": { "Transform": 1, "Script": 2 },
		"Entities": [
			{ "ID": "00000000000f0001", "Name": "Turret", "Parent": null, "Active": true, "Tags": [],
			  "Components": {
				"Transform": { "Translation": [0, 0, 0], "Rotation": [0, 0, 0, 1], "Scale": [1, 1, 1] },
				"Script": { "Scripts": [
					{ "Script": "00000000c0ffee01", "Fields": { "Torque": 10 }, "ExecutionOrder": 0 },
					{ "Script": "00000000c0ffee02", "Fields": { "Count": 3 }, "ExecutionOrder": 5 } ] } } }
		]
	})";

	// A hologram projector whose root and child carry the unknown component "Hologram" at version 3.
	constexpr std::string_view HologramPrefab = R"({
		"Format": "Prefab", "Version": 1, "Name": "Projector", "Root": "00000000000e0001",
		"ComponentVersions": { "Transform": 1, "Hologram": 3 },
		"Entities": [
			{ "ID": "00000000000e0001", "Name": "Projector", "Parent": null, "Active": true, "Tags": [],
			  "Components": { "Transform": {}, "Hologram": { "Flicker": 0.5 } } },
			{ "ID": "00000000000e0002", "Name": "Beam", "Parent": "00000000000e0001", "Active": true, "Tags": [],
			  "Components": { "Transform": {}, "Hologram": { "Flicker": 0.25 } } }
		]
	})";

	// The unknown component `name` that `entity` preserves, or nullptr.
	static const UnknownComponentData* FindUnknown(ConstEntity entity, std::string_view name)
	{
		const UnknownComponentsComponent* preserved = entity.TryGetComponent<UnknownComponentsComponent>();
		if (preserved == nullptr)
			return nullptr;
		const auto found = std::find_if(preserved->Components.begin(), preserved->Components.end(), [name](const UnknownComponentData& data)
		{
			return data.Name == name;
		});
		return found != preserved->Components.end() ? &*found : nullptr;
	}

	// A sky: one entity with the unique-per-scene Environment.
	constexpr std::string_view SkyPrefab = R"({
		"Format": "Prefab", "Version": 1, "Name": "Sky", "Root": "0000000000010001", "ComponentVersions": {},
		"Entities": [ { "ID": "0000000000010001", "Name": "Sky", "Parent": null, "Active": true, "Tags": [],
		                "Components": { "Transform": {}, "Environment": {} } } ]
	})";

	TEST_SUITE("Scene")
	{
		TEST_CASE("Prefab: instance IDs are Hash64(root, prefabEntity)")
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

		TEST_CASE("Prefab: field override survives a prefab edit")
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
			CHECK(overrides[0].Path == std::vector<UUID>{ UUID(0x00000000000b0002) });
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

		TEST_CASE("Prefab: external EntityRef into an instance survives update")
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x2222)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID target = root->GetChildren()[0];

			// An entity outside the instance references the member through a script field (an Entity-kind Variant).
			const Entity watcher = setup.GetScene().CreateEntity("Watcher");
			ScriptSlot& script = watcher.AddComponent<ScriptComponent>().Scripts.emplace_back();
			script.Fields["Target"] = VariantValue(Json(target.ToString()));

			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, edited, PrefabOptions{}, report).has_value());
			const Entity resolved = setup.GetScene().FindEntityByID(target);
			REQUIRE(resolved.IsValid());
			CHECK(resolved.GetName() == "Visual");
			CHECK(watcher.GetComponent<ScriptComponent>().Scripts.front().Fields.at("Target").Get() == Json(target.ToString()));
		}

		TEST_CASE("Prefab: user-added children are preserved")
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

			// Overrides do not record user children: against the prefab the instance now follows, it has none.
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, edited, PrefabOptions{});
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());
		}

		TEST_CASE("PrefabInstantiator: internal references are remapped to the derived IDs")
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

		TEST_CASE("Prefab: a deactivated member stays inactive across updates")
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
			CHECK(overrides[0].Path == std::vector<UUID>{ UUID(0x00000000000b0002) });
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

		TEST_CASE("PrefabInstantiator: Entity-kind script fields inside the prefab follow the derived IDs")
		{
			Test::SceneTestFixture setup;
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;

			// The root's script field "Goal" names the prefab-local ID of the barrel.
			const Prefab prefab = ParsePrefab(setup.GetRegistry(), TurretPrefab, &schemas);

			const UUID rootID(0x8888);
			const Json barrel(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000d0002)).ToString());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());
			CHECK(root->GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == barrel);

			// A fresh instance has no overrides: the prefab side is remapped before the comparison.
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, prefab, options);
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());

			// Rebuilding the instance keeps the reference on the derived member.
			REQUIRE(PrefabInstantiator::UpdateInstance(setup.GetScene(), *root, prefab, options, report).has_value());
			const Entity rebuilt = setup.GetScene().FindEntityByID(rootID);
			REQUIRE(rebuilt.IsValid());
			CHECK(rebuilt.GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == barrel);
		}

		TEST_CASE("PrefabInstantiator: unpack removes the links and keeps the entities")
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

		TEST_CASE("PrefabInstantiator: a used root ID or an empty prefab is rejected")
		{
			Test::SceneTestFixture setup;
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			static_cast<void>(setup.GetScene().CreateEntityWithID(UUID(0x6666), "Taken"));
			LoadReport report;
			CHECK(GetErrorCode(PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID()), PrefabOptions{}, report))
				== ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::Instantiate(setup.GetScene(), Prefab(), MakeOptions(UUID(0x7777)), PrefabOptions{}, report))
				== ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(UUID(0x6666)), PrefabOptions{}, report))
				== ErrorCode::AlreadyExists);

			// A member's derived ID that is taken is an inconsistent scene rather than the caller's choice.
			const UUID rootID(0x6767);
			const UUID squatted = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002));
			static_cast<void>(setup.GetScene().CreateEntityWithID(squatted, "Squatter"));
			CHECK(GetErrorCode(PrefabInstantiator::Instantiate(setup.GetScene(), prefab, MakeOptions(rootID), PrefabOptions{}, report))
				== ErrorCode::InvalidState);
			CHECK(setup.GetScene().GetEntityCount() == 2);
		}

		TEST_CASE("PrefabInstantiator: component and entity-key overrides are recorded, survive an edit, apply back and revert")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const UUID rootID(0xaaaa);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002));
			Entity visual = scene.FindEntityByID(visualID);
			visual.RemoveComponent<MeshRendererComponent>();
			SphereColliderComponent collider;
			collider.Radius = 2.0f;
			visual.AddComponent<SphereColliderComponent>(collider);
			root->AddTag("Spawner");

			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 3);
			CHECK(recorded[0].Path == std::vector<UUID>{ UUID(0x00000000000b0001) });
			CHECK(recorded[0].Kind == PrefabOverrideKind::EntityKey);
			CHECK(recorded[0].Field == "Tags");
			CHECK(recorded[0].Value.Get() == Json::array({ "Spawner" }));
			CHECK(recorded[1].Kind == PrefabOverrideKind::RemoveComponent);
			CHECK(recorded[1].Component == "MeshRenderer");
			CHECK(recorded[1].Value.IsNull());
			CHECK(recorded[2].Kind == PrefabOverrideKind::AddComponent);
			CHECK(recorded[2].Component == "SphereCollider");
			CHECK(recorded[2].Value.Get()["Radius"] == 2);

			// The edit changed the mesh the instance removed: the overrides still win.
			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), prefab);
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, edited, PrefabOptions{}, report).has_value());
			visual = scene.FindEntityByID(visualID);
			REQUIRE(visual.IsValid());
			CHECK_FALSE(visual.HasComponent<MeshRendererComponent>());
			REQUIRE(visual.HasComponent<SphereColliderComponent>());
			CHECK(visual.GetComponent<SphereColliderComponent>().Radius == 2.0f);
			CHECK(root->HasTag("Spawner"));
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 3);
			CHECK(CountDiagnostics(report, PrefabStaleOverrideCode) == 0);

			// prefab.apply: the instance's data becomes the prefab's, in prefab-local IDs and without the link components; the
			// root keeps the prefab's own Name and Transform.
			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, edited, PrefabOptions{});
			REQUIRE(applied.has_value());
			CHECK(applied->GetRootID() == UUID(0x00000000000b0001));
			CHECK(applied->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000b0001), UUID(0x00000000000b0002), UUID(0x00000000000b0003) });
			const Json* appliedRoot = applied->FindEntity(UUID(0x00000000000b0001));
			const Json* appliedVisual = applied->FindEntity(UUID(0x00000000000b0002));
			REQUIRE(appliedRoot != nullptr);
			REQUIRE(appliedVisual != nullptr);
			CHECK((*appliedRoot)["Tags"] == Json::array({ "Spawner" }));
			CHECK((*appliedRoot)["Name"] == "Block");
			CHECK_FALSE((*appliedRoot)["Components"].contains("Prefab"));
			CHECK((*appliedVisual)["Parent"] == "00000000000b0001");
			CHECK_FALSE((*appliedVisual)["Components"].contains("MeshRenderer"));
			CHECK_FALSE((*appliedVisual)["Components"].contains("PrefabLink"));
			CHECK((*appliedVisual)["Components"]["SphereCollider"]["Radius"] == 2);
			const Result<std::vector<PrefabOverride>> afterApply = PrefabInstantiator::ComputeOverrides(*root, *applied, PrefabOptions{});
			REQUIRE(afterApply.has_value());
			CHECK(afterApply->empty());

			// prefab.revert: the instance follows the prefab again.
			REQUIRE(PrefabInstantiator::Revert(scene, *root, edited, PrefabOptions{}, report).has_value());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
			visual = scene.FindEntityByID(visualID);
			REQUIRE(visual.IsValid());
			CHECK(visual.HasComponent<MeshRendererComponent>());
			CHECK_FALSE(visual.HasComponent<SphereColliderComponent>());
			CHECK_FALSE(root->HasTag("Spawner"));
		}

		TEST_CASE("PrefabInstantiator: overrides that no longer match the prefab are dropped with a warning")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(UUID(0xbbbb)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID visual(0x00000000000b0002);
			root->Patch<PrefabInstanceComponent>([visual](PrefabInstanceComponent& instance)
			{
				instance.Overrides = {
					MakeOverride(visual, PrefabOverrideKind::Field, "Transform", "Scale", Json::array({ 1, 3, 1 })),
					MakeOverride(visual, PrefabOverrideKind::Field, "MeshRenderer", "Nope", Json(true)),
					MakeOverride(visual, PrefabOverrideKind::Field, "Transform", "Translation", Json::array({ 0, "up", 0 })),
					MakeOverride(UUID(0x00000000000b0009), PrefabOverrideKind::Field, "Transform", "Scale", Json::array({ 2, 2, 2 })),
					MakeOverride(UUID(0x00000000000b0001), PrefabOverrideKind::Field, "Transform", "Scale", Json::array({ 2, 2, 2 })),
					MakeOverride(visual, PrefabOverrideKind::RemoveComponent, "Transform", "", Json()),
				};
			});

			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, prefab, PrefabOptions{}, updateReport).has_value());
			CHECK(CountDiagnostics(updateReport, PrefabStaleOverrideCode) == 5);
			const std::vector<PrefabOverride>& kept = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(kept.size() == 1);
			CHECK(kept[0].Field == "Scale");
			const Entity member = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(UUID(0xbbbb), visual));
			REQUIRE(member.IsValid());
			CHECK(member.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f, 3.0f, 1.0f));
			CHECK(root->GetComponent<TransformComponent>().Scale == glm::vec3(1.0f));
		}

		TEST_CASE("PrefabInstantiator: members whose prefab entity is gone disappear and their user children move up")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab chain = ParsePrefab(setup.GetRegistry(), ChainPrefab);
			LoadReport report;
			const UUID rootID(0xcccc);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, chain, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID a = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0002));
			const UUID b = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0003));
			const Entity user = scene.CreateEntity("User", scene.FindEntityByID(b));
			user.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 0.0f, 0.0f);
			});
			CHECK(scene.GetEntityCount() == 4);
			CHECK(TransformSystem::GetWorldPosition(user) == glm::vec3(1.0f, 2.0f, 0.0f));

			// The edited prefab no longer has B.
			const Prefab shorter = RemovePrefabEntities(setup.GetRegistry(), chain, { 2 });
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, shorter, PrefabOptions{}, report).has_value());
			CHECK_FALSE(scene.FindEntityByID(b).IsValid());
			REQUIRE(user.IsValid());
			CHECK(user.GetParent().GetUUID() == a);
			CHECK(scene.GetEntityCount() == 3);

			// The user child keeps its local transform, so it moves with B's offset gone.
			CHECK(user.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 0.0f, 0.0f));
			CHECK(TransformSystem::GetWorldPosition(user) == glm::vec3(1.0f, 1.0f, 0.0f));
		}

		TEST_CASE("PrefabInstantiator: a member that disappears with its member descendants takes only those along")
		{
			for (const bool runtime : { false, true })
			{
				CAPTURE(runtime);
				Test::SceneTestFixture setup(1, runtime);
				Scene& scene = setup.GetScene();
				const Prefab chain = ParsePrefab(setup.GetRegistry(), ChainPrefab);
				LoadReport report;
				const UUID rootID(0xcdcd);
				const Result<Entity> root = PrefabInstantiator::Instantiate(scene, chain, MakeOptions(rootID), PrefabOptions{}, report);
				REQUIRE(root.has_value());
				const UUID a = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0002));
				const UUID b = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0003));
				const Entity user = scene.CreateEntity("User", scene.FindEntityByID(b));

				// The edited prefab has only its root: A goes, and B goes with it.
				const Prefab rootOnly = RemovePrefabEntities(setup.GetRegistry(), chain, { 2, 1 });
				REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, rootOnly, PrefabOptions{}, report).has_value());
				CHECK_FALSE(scene.FindEntityByID(a).IsValid());
				CHECK_FALSE(scene.FindEntityByID(b).IsValid());
				REQUIRE(user.IsValid());
				CHECK(user.GetParent() == *root);
				CHECK(root->GetChildren().size() == 1);
				CHECK(scene.GetEntityCount() == 2);
				const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, rootOnly, PrefabOptions{});
				REQUIRE(overrides.has_value());
				CHECK(overrides->empty());
			}
		}

		TEST_CASE("PrefabInstantiator: updating from an unchanged prefab changes nothing")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(UUID(0xdddd)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity visual = scene.FindEntityByID(root->GetChildren()[0]);
			static_cast<void>(scene.CreateEntity("User", *root));
			visual.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 4.0f, 0.0f);
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<UUID> children(root->GetChildren().begin(), root->GetChildren().end());
			const uint64_t revision = scene.GetRevision();

			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, prefab, PrefabOptions{}, report).has_value());
			CHECK(scene.GetRevision() == revision);
			CHECK(std::vector<UUID>(root->GetChildren().begin(), root->GetChildren().end()) == children);
			CHECK(visual.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f, 4.0f, 0.0f));
		}

		TEST_CASE("PrefabInstantiator: the parent, sibling index and root transform place the instance")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			const Entity holder = scene.CreateEntity("Holder");
			const Entity existing = scene.CreateEntity("Existing", holder);

			PrefabInstantiateOptions options = MakeOptions(UUID(0xeeee));
			options.Parent = holder;
			options.SiblingIndex = 0;
			TransformComponent transform;
			transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			options.RootTransform = transform;
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, options, PrefabOptions{}, report);
			REQUIRE(root.has_value());
			CHECK(root->GetParent() == holder);
			REQUIRE(holder.GetChildren().size() == 2);
			CHECK(holder.GetChildren()[0] == root->GetUUID());
			CHECK(holder.GetChildren()[1] == existing.GetUUID());
			CHECK(root->GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*root, prefab, PrefabOptions{});
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty()); // the root's Transform is an implicit override

			// An invalid root transform is rejected before anything is created.
			const size_t count = scene.GetEntityCount();
			const uint64_t revision = scene.GetRevision();
			PrefabInstantiateOptions invalid = MakeOptions(UUID(0xffff));
			TransformComponent flat;
			flat.Scale = glm::vec3(1.0f, 0.0f, 1.0f);
			invalid.RootTransform = flat;
			const Result<Entity> rejected = PrefabInstantiator::Instantiate(scene, prefab, invalid, PrefabOptions{}, report);
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().GetCode() == ErrorCode::Validation);
			CHECK(scene.GetEntityCount() == count);
			CHECK(scene.GetRevision() == revision);
		}

		TEST_CASE("PrefabInstantiator: a unique-per-scene component the scene already has is rejected")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab sky = ParsePrefab(setup.GetRegistry(), SkyPrefab);
			LoadReport report;
			REQUIRE(PrefabInstantiator::Instantiate(scene, sky, MakeOptions(UUID(0x1001)), PrefabOptions{}, report).has_value());

			const size_t count = scene.GetEntityCount();
			const uint64_t revision = scene.GetRevision();
			const Result<Entity> second = PrefabInstantiator::Instantiate(scene, sky, MakeOptions(UUID(0x1002)), PrefabOptions{}, report);
			REQUIRE_FALSE(second.has_value());
			CHECK(second.error().GetCode() == ErrorCode::Validation);
			CHECK(scene.GetEntityCount() == count);
			CHECK(scene.GetRevision() == revision);
		}

		TEST_CASE("PrefabInstantiator: updates check unique-per-scene components after the instance's overrides")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab sky = ParsePrefab(setup.GetRegistry(), SkyPrefab);
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, sky, MakeOptions(UUID(0x1003)), PrefabOptions{}, report);
			REQUIRE(root.has_value());

			// The instance gave its Environment up to another entity: an update keeps it that way.
			root->RemoveComponent<EnvironmentComponent>();
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, sky, PrefabOptions{}).has_value());
			const Entity sun = scene.CreateEntity("Sun");
			static_cast<void>(sun.AddComponent<EnvironmentComponent>());
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, sky, PrefabOptions{}, updateReport).has_value());
			CHECK(updateReport.Diagnostics.empty());
			CHECK_FALSE(root->HasComponent<EnvironmentComponent>());
			CHECK(sun.HasComponent<EnvironmentComponent>());

			// Reverting would give the scene a second Environment: rejected, with nothing changed or reported.
			const uint64_t revision = scene.GetRevision();
			LoadReport revertReport;
			CHECK(GetErrorCode(PrefabInstantiator::Revert(scene, *root, sky, PrefabOptions{}, revertReport)) == ErrorCode::Validation);
			CHECK(revertReport.Diagnostics.empty());
			CHECK(scene.GetRevision() == revision);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
			CHECK_FALSE(root->HasComponent<EnvironmentComponent>());
		}

		TEST_CASE("PrefabInstantiator: a scene loads its expanded instances and overrides while the prefab asset is missing")
		{
			// Scenes store instances fully expanded plus their override records (§5.5), and loading never resolves a prefab
			// handle, so a scene whose prefab asset is gone loads unchanged. Reporting the missing asset itself
			// (PREFAB_MISSING_ASSET) belongs to asset validation, which knows the asset registry; nothing here reads the prefab.
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const UUID rootID(0x5a5a);
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002));
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity visual = scene.FindEntityByID(visualID);
			visual.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 2.0f, 1.0f);
			});
			visual.SetActive(false);
			const Entity user = scene.CreateEntity("User", visual);
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 2);
			const Result<std::string> saved = SceneSerializer::SaveToString(scene);
			REQUIRE(saved.has_value());

			// Strict, with unknowns as errors: the file is all the load needs.
			const Scope<Scene> loaded = setup.CreateEmptyScene();
			LoadOptions strict;
			strict.StrictUnknowns = true;
			LoadReport loadReport;
			REQUIRE(SceneSerializer::LoadFromString(*loaded, *saved, strict, loadReport).has_value());
			CHECK(loadReport.Diagnostics.empty());
			CHECK(loadReport.Repairs.empty());

			const Entity loadedRoot = loaded->FindEntityByID(rootID);
			const Entity loadedVisual = loaded->FindEntityByID(visualID);
			const Entity loadedUser = loaded->FindEntityByID(user.GetUUID());
			REQUIRE(loadedRoot.IsValid());
			REQUIRE(loadedVisual.IsValid());
			REQUIRE(loadedUser.IsValid());
			const PrefabInstanceComponent& instance = loadedRoot.GetComponent<PrefabInstanceComponent>();
			CHECK(instance.Prefab.GetHandle() == AssetHandle(0x3c9f2e7a11d04b88));
			REQUIRE(instance.Overrides.size() == recorded.size());
			for (size_t index = 0; index < recorded.size(); ++index)
			{
				CHECK(instance.Overrides[index].Path == recorded[index].Path);
				CHECK(instance.Overrides[index].Kind == recorded[index].Kind);
				CHECK(instance.Overrides[index].Component == recorded[index].Component);
				CHECK(instance.Overrides[index].Field == recorded[index].Field);
				CHECK(instance.Overrides[index].Value == recorded[index].Value);
			}
			CHECK(loadedVisual.GetComponent<PrefabLinkComponent>().PrefabEntityID == UUID(0x00000000000b0002));
			CHECK(loadedVisual.GetComponent<PrefabLinkComponent>().InstanceRoot == rootID);
			CHECK(loadedVisual.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == AssetHandle(0x0000000000000101)); // prefab data
			CHECK(loadedVisual.GetComponent<TransformComponent>().Scale == glm::vec3(1.0f, 2.0f, 1.0f));                   // an override
			CHECK_FALSE(loadedVisual.IsActiveSelf());                                                                      // an override
			CHECK(loadedUser.GetParent() == loadedVisual);
			const Result<std::string> resaved = SceneSerializer::SaveToString(*loaded);
			REQUIRE(resaved.has_value());
			CHECK(*resaved == *saved);

			// Once the asset is back, updating from it changes nothing: the stored instance is the prefab plus its overrides.
			const uint64_t revision = loaded->GetRevision();
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(*loaded, loadedRoot, prefab, PrefabOptions{}, updateReport).has_value());
			CHECK(updateReport.Diagnostics.empty());
			CHECK(loaded->GetRevision() == revision);
		}

		TEST_CASE("PrefabInstantiator: instantiation, override refreshes and updates are recorded by the change tracker")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			ChangeTracker& tracker = scene.GetChangeTracker();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			const Entity bystander = scene.CreateEntity("Bystander");
			const UUID rootID(0x7a7a);
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0002));
			LoadReport report;

			tracker.Begin();
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const std::vector<EntityChange> created = tracker.End();
			REQUIRE(created.size() == 2);
			REQUIRE(FindChange(created, rootID) != nullptr);
			REQUIRE(FindChange(created, visualID) != nullptr);
			CHECK(FindChange(created, rootID)->Kind == EntityChangeKind::Created);
			CHECK(FindChange(created, visualID)->Kind == EntityChangeKind::Created);

			// An edit of a member commits together with the overrides it implies, so one undo step restores both.
			tracker.Begin();
			scene.FindEntityByID(visualID).Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 2.0f, 1.0f);
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, PrefabOptions{}).has_value());
			const std::vector<EntityChange> edited = tracker.End();
			REQUIRE(edited.size() == 2);
			const EntityChange* rootChange = FindChange(edited, rootID);
			const EntityChange* visualChange = FindChange(edited, visualID);
			REQUIRE(rootChange != nullptr);
			REQUIRE(visualChange != nullptr);
			CHECK(rootChange->Kind == EntityChangeKind::Modified);
			CHECK(rootChange->Components == std::vector<std::string>{ "Prefab" });
			REQUIRE(rootChange->Before != nullptr);
			CHECK((*rootChange->Before)["Components"]["Prefab"]["Overrides"] == Json::array());
			CHECK(visualChange->Kind == EntityChangeKind::Modified);
			CHECK(visualChange->Components == std::vector<std::string>{ "Transform" });

			// A prefab update touches the instance only: the member whose data changed and the new member.
			const Prefab edit = EditBlockPrefab(setup.GetRegistry(), prefab);
			tracker.Begin();
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, edit, PrefabOptions{}, report).has_value());
			const std::vector<EntityChange> updated = tracker.End();
			const EntityChange* meshChange = FindChange(updated, visualID);
			const EntityChange* lightChange = FindChange(updated, PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000b0003)));
			REQUIRE(meshChange != nullptr);
			REQUIRE(lightChange != nullptr);
			CHECK(meshChange->Kind == EntityChangeKind::Modified);
			CHECK(meshChange->Components == std::vector<std::string>{ "MeshRenderer" });
			CHECK(lightChange->Kind == EntityChangeKind::Created);
			CHECK(updated.size() == 2);
			CHECK(FindChange(updated, bystander.GetUUID()) == nullptr);
		}

		TEST_CASE("PrefabInstantiator: apply maps internal references back to prefab-local IDs and leaves user children out")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;
			const Prefab turret = ParsePrefab(setup.GetRegistry(), TurretPrefab, &schemas);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const UUID barrelID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000d0002));
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, turret, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());

			// The barrel gets a script aiming at the turret root (inside the instance); the root now aims at a user child
			// (outside it).
			const Entity muzzle = scene.CreateEntity("Muzzle", scene.FindEntityByID(barrelID));
			ScriptSlot aim;
			aim.Script = TypedAssetHandle<AssetType::Script>(AssetHandle(Test::FixtureSchemaSource::DefaultOwner.GetValue()));
			aim.Fields["Goal"] = VariantValue(Json(rootID.ToString()));
			static_cast<void>(scene.FindEntityByID(barrelID).AddComponent<ScriptComponent>(ScriptComponent{ { aim } }));
			root->Patch<ScriptComponent>([&muzzle](ScriptComponent& script)
			{
				script.Scripts.front().Fields["Goal"] = VariantValue(Json(muzzle.GetUUID().ToString()));
			});

			// Overrides store prefab-local IDs for members and keep references to other entities as they are.
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, turret, options).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 2);
			CHECK(recorded[0].Path == std::vector<UUID>{ UUID(0x00000000000d0001) });
			CHECK(recorded[0].Kind == PrefabOverrideKind::Field);
			CHECK(recorded[0].Component == "Script");
			CHECK(recorded[0].Field == "Scripts/0/Fields");
			CHECK(recorded[0].Value.Get()["Goal"] == Json(muzzle.GetUUID().ToString()));
			CHECK(recorded[1].Path == std::vector<UUID>{ UUID(0x00000000000d0002) });
			CHECK(recorded[1].Kind == PrefabOverrideKind::AddComponent);
			CHECK(recorded[1].Value.Get()["Scripts"][0]["Fields"]["Goal"] == "00000000000d0001");

			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, turret, options);
			REQUIRE(applied.has_value());
			CHECK(applied->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000d0001), UUID(0x00000000000d0002) });
			const Json* appliedRoot = applied->FindEntity(UUID(0x00000000000d0001));
			const Json* appliedBarrel = applied->FindEntity(UUID(0x00000000000d0002));
			REQUIRE(appliedRoot != nullptr);
			REQUIRE(appliedBarrel != nullptr);
			CHECK((*appliedRoot)["Components"]["Script"]["Scripts"][0]["Fields"]["Goal"] == Json(muzzle.GetUUID().ToString()));
			CHECK((*appliedBarrel)["Components"]["Script"]["Scripts"][0]["Fields"]["Goal"] == "00000000000d0001");

			// The caller saves the asset and updates its instances; the applied instance reverts to it: no overrides remain,
			// nothing changes and the user child stays.
			REQUIRE(PrefabInstantiator::Revert(scene, *root, *applied, options, report).has_value());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
			const Entity barrel = scene.FindEntityByID(barrelID);
			REQUIRE(barrel.IsValid());
			CHECK(barrel.GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == Json(rootID.ToString()));
			CHECK(root->GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == Json(muzzle.GetUUID().ToString()));
			REQUIRE(muzzle.IsValid());
			CHECK(muzzle.GetParent() == barrel);
			const Result<std::vector<PrefabOverride>> remaining = PrefabInstantiator::ComputeOverrides(*root, *applied, options);
			REQUIRE(remaining.has_value());
			CHECK(remaining->empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (E-PFB-NEST): creating a prefab flattened the instances inside it.
		// An instance wholly inside the subtree stays live in the new prefab, outermost there; the instance of the subtree's
		// root is unpacked in the copy. An instance of the new prefab holds the nested instance as an instance of its own
		// prefab at a derived root id, whose OuterPrefabEntityID names its id in the new prefab, and has no overrides.
		TEST_CASE("Prefab: CreateFromEntity keeps a nested instance live")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const UUID outerID(0x6c6c);
			const UUID innerID(0x6d6d);
			const Result<Entity> outer = PrefabInstantiator::Instantiate(scene, block, MakeOptions(outerID), PrefabOptions{}, report);
			REQUIRE(outer.has_value());
			PrefabInstantiateOptions innerOptions = MakeOptions(innerID);
			innerOptions.Parent = *outer;
			REQUIRE(PrefabInstantiator::Instantiate(scene, block, innerOptions, PrefabOptions{}, report).has_value());

			const Result<Prefab> tower = Prefab::CreateFromEntity(*outer, "Tower");
			REQUIRE(tower.has_value());
			const UUID outerVisual = PrefabInstantiator::DeriveInstanceID(outerID, UUID(0x00000000000b0002));
			const UUID innerVisual = PrefabInstantiator::DeriveInstanceID(innerID, UUID(0x00000000000b0002));
			CHECK(tower->GetEntityIDs() == std::vector<UUID>{ outerID, outerVisual, innerID, innerVisual });
			// The root's own instance is unpacked; the inner instance stays live, outermost in the document.
			for (const UUID id : { outerID, outerVisual })
			{
				const Json* entity = tower->FindEntity(id);
				REQUIRE(entity != nullptr);
				CHECK_FALSE((*entity)["Components"].contains("Prefab"));
				CHECK_FALSE((*entity)["Components"].contains("PrefabLink"));
			}
			const Json* innerRoot = tower->FindEntity(innerID);
			REQUIRE(innerRoot != nullptr);
			REQUIRE((*innerRoot)["Components"].contains("Prefab"));
			CHECK((*innerRoot)["Components"]["Prefab"]["Overrides"] == Json::array());
			CHECK((*innerRoot)["Components"]["PrefabLink"]["InstanceRoot"] == Json(innerID.ToString()));
			CHECK((*innerRoot)["Components"]["PrefabLink"]["OuterPrefabEntityID"] == Json(nullptr));
			const Json* innerMember = tower->FindEntity(innerVisual);
			REQUIRE(innerMember != nullptr);
			CHECK((*innerMember)["Components"]["PrefabLink"]["InstanceRoot"] == Json(innerID.ToString()));
			CHECK((*innerMember)["Components"]["PrefabLink"]["PrefabEntityID"] == Json(UUID(0x00000000000b0002).ToString()));

			// An instance of the tower: its own members link to its root; the nested instance is a Block instance at
			// Hash64(tower root, its id in the tower), its members derived from that root.
			const UUID towerID(0x6e6e);
			const Result<Entity> instance = PrefabInstantiator::Instantiate(scene, *tower, MakeOptions(towerID), PrefabOptions{}, report);
			REQUIRE(instance.has_value());
			const Entity towerVisual = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(towerID, outerVisual));
			REQUIRE(towerVisual.IsValid());
			CHECK(towerVisual.GetComponent<PrefabLinkComponent>().InstanceRoot == towerID);
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(towerID, innerID);
			const Entity nested = scene.FindEntityByID(nestedID);
			REQUIRE(nested.IsValid());
			CHECK(nested.HasComponent<PrefabInstanceComponent>());
			CHECK(nested.GetComponent<PrefabLinkComponent>().InstanceRoot == nestedID);
			CHECK(nested.GetComponent<PrefabLinkComponent>().PrefabEntityID == UUID(0x00000000000b0001));
			const Result<Json> nestedJson = SceneSerializer::EntityToJson(nested);
			REQUIRE(nestedJson.has_value());
			CHECK((*nestedJson)["Components"]["PrefabLink"]["OuterPrefabEntityID"] == Json(innerID.ToString()));
			const Entity nestedVisual = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002)));
			REQUIRE(nestedVisual.IsValid());
			CHECK(nestedVisual.GetComponent<PrefabLinkComponent>().InstanceRoot == nestedID);
			const Result<std::vector<PrefabOverride>> overrides = PrefabInstantiator::ComputeOverrides(*instance, *tower, PrefabOptions{});
			REQUIRE(overrides.has_value());
			CHECK(overrides->empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 8: an override targets an entity by its path from the outermost
		// instance: the nested root's id in the outer prefab, then the member's id in the nested prefab. The levels of a
		// nested member are its outermost root, then its nested root.
		TEST_CASE("PrefabInstantiator: a nested member's override path names the nested root and the member")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			// The prefab "Shelf": a root holding a live Block instance at the shelf-local id 0x7a7a.
			Entity shelfRoot = scene.CreateEntityWithID(UUID(0x7979), "Shelf");
			PrefabInstantiateOptions nestedOptions = MakeOptions(UUID(0x7a7a));
			nestedOptions.Parent = shelfRoot;
			REQUIRE(PrefabInstantiator::Instantiate(scene, block, nestedOptions, PrefabOptions{}, report).has_value());
			const Result<Prefab> shelf = Prefab::CreateFromEntity(shelfRoot, "Shelf");
			REQUIRE(shelf.has_value());

			const UUID rootID(0x7b7b);
			const Scope<Scene> other = setup.CreateEmptyScene();
			const Result<Entity> root = PrefabInstantiator::Instantiate(*other, *shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a));
			const UUID memberID = PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002));
			const Entity nested = other->FindEntityByID(nestedID);
			const Entity member = other->FindEntityByID(memberID);
			REQUIRE(nested.IsValid());
			REQUIRE(member.IsValid());

			const Result<std::vector<UUID>> memberPath = PrefabInstantiator::GetOverridePath(member);
			REQUIRE_MESSAGE(memberPath.has_value(), memberPath.error().ToString());
			CHECK(*memberPath == std::vector<UUID>{ UUID(0x7a7a), UUID(0x00000000000b0002) });
			const Result<std::vector<UUID>> nestedPath = PrefabInstantiator::GetOverridePath(nested);
			REQUIRE(nestedPath.has_value());
			CHECK(*nestedPath == std::vector<UUID>{ UUID(0x7a7a) });
			const Result<std::vector<UUID>> rootPath = PrefabInstantiator::GetOverridePath(*root);
			REQUIRE(rootPath.has_value());
			CHECK(*rootPath == std::vector<UUID>{ UUID(0x7979) });

			const Result<std::vector<Entity>> levels = PrefabInstantiator::GetInstanceLevels(member);
			REQUIRE(levels.has_value());
			REQUIRE(levels->size() == 2);
			CHECK((*levels)[0] == *root);
			CHECK((*levels)[1] == nested);
			const Result<std::vector<Entity>> outerLevels = PrefabInstantiator::GetInstanceLevels(*root);
			REQUIRE(outerLevels.has_value());
			CHECK(*outerLevels == std::vector<Entity>{ *root });
			const Entity userChild = other->CreateEntity("Note", *root);
			const Result<std::vector<Entity>> noLevels = PrefabInstantiator::GetInstanceLevels(userChild);
			REQUIRE(noLevels.has_value());
			CHECK(noLevels->empty());

			// An edit of the nested member is recorded on the outermost root with that path; the nested root records none.
			member.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.CastShadows = false;
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, *shelf, PrefabOptions{}).has_value());
			const Result<Json> rootJson = SceneSerializer::EntityToJson(*root);
			REQUIRE(rootJson.has_value());
			const Json& recorded = (*rootJson)["Components"]["Prefab"]["Overrides"];
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0]["Path"] == Json::array({ UUID(0x7a7a).ToString(), UUID(0x00000000000b0002).ToString() }));
			CHECK(recorded[0]["Field"] == Json("CastShadows"));
			CHECK(nested.GetComponent<PrefabInstanceComponent>().Overrides.empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 8 (Roadmap M20 acceptance "inner edits propagate"): a new version of a
		// nested prefab reaches a document nesting it, whose nested instance is rebuilt from it keeping the record the
		// document holds for it, and then every instance of that document, whose own overrides of the nested instance's
		// entities (recorded on the outermost root by path) stay in force.
		TEST_CASE("PrefabInstantiator: a nested prefab's new version reaches the prefab nesting it and its instances, every record kept")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			CHECK(GetDocumentInstance(shelf, UUID(0x7a7a))["Overrides"].size() == 1);

			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a));
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002));
			const Entity visual = scene.FindEntityByID(visualID);
			REQUIRE(visual.IsValid());
			CHECK_FALSE(visual.GetComponent<MeshRendererComponent>().ReceiveShadows);
			CHECK(scene.FindEntityByID(nestedID).GetComponent<PrefabInstanceComponent>().Overrides.empty());

			// The scene's instance moves the nested Visual: recorded on the outermost root by its path.
			visual.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, shelf, PrefabOptions{}).has_value());
			REQUIRE(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides[0].Path == std::vector<UUID>{ UUID(0x7a7a), UUID(0x00000000000b0002) });

			// Block gets a new version (another mesh, no shadows cast, a new child Light): the shelf's document is rewritten.
			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), block);
			const std::map<AssetHandle, Prefab> versions{ { AssetHandle(0x3c9f2e7a11d04b88), edited } };
			LoadReport updateReport;
			const Result<Prefab> rewritten = UpdateNestedPrefabInstances(shelf, setup.GetRegistry(), versions, PrefabOptions{}, updateReport);
			REQUIRE_MESSAGE(rewritten.has_value(), (rewritten ? std::string() : rewritten.error().ToString()));
			const Json* rewrittenVisual = rewritten->FindEntity(PrefabInstantiator::DeriveInstanceID(UUID(0x7a7a), UUID(0x00000000000b0002)));
			REQUIRE(rewrittenVisual != nullptr);
			CHECK((*rewrittenVisual)["Components"]["MeshRenderer"]["Mesh"] == Json("0000000000000105"));
			CHECK((*rewrittenVisual)["Components"]["MeshRenderer"]["ReceiveShadows"] == Json(false));
			CHECK(rewritten->FindEntity(PrefabInstantiator::DeriveInstanceID(UUID(0x7a7a), UUID(0x00000000000b0003))) != nullptr);
			CHECK(GetDocumentInstance(*rewritten, UUID(0x7a7a))["Overrides"].size() == 1);
			// A document whose nested prefabs did not change is rewritten as it was.
			const Result<Prefab> unchanged = UpdateNestedPrefabInstances(shelf, setup.GetRegistry(), {}, PrefabOptions{}, updateReport);
			REQUIRE(unchanged.has_value());
			const Result<std::string> unchangedText = unchanged->SaveToString();
			const Result<std::string> shelfText = shelf.SaveToString();
			REQUIRE(unchangedText.has_value());
			REQUIRE(shelfText.has_value());
			CHECK(*unchangedText == *shelfText);

			// The scene's instance follows the rewritten shelf: the new mesh and child, the shelf's record and its own move.
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *rewritten, PrefabOptions{}, report).has_value());
			const Entity updated = scene.FindEntityByID(visualID);
			REQUIRE(updated.IsValid());
			CHECK(updated.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == AssetHandle(0x105));
			CHECK_FALSE(updated.GetComponent<MeshRendererComponent>().ReceiveShadows);
			CHECK_FALSE(updated.GetComponent<MeshRendererComponent>().CastShadows);
			CHECK(updated.GetComponent<TransformComponent>().Translation == glm::vec3(1.0f, 2.0f, 3.0f));
			const Entity light = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0003)));
			REQUIRE(light.IsValid());
			CHECK(light.GetComponent<PrefabLinkComponent>().InstanceRoot == nestedID);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);
			CHECK(scene.FindEntityByID(nestedID).GetComponent<PrefabInstanceComponent>().Overrides.empty());
		}

		// Decision 8: an instance is rebuilt, diffed and applied from its outermost root only, so the operations refuse a
		// nested root, naming the root to use; a nested root whose enclosing instance is gone has no levels.
		TEST_CASE("PrefabInstantiator: a nested instance root refuses its outermost instance's operations and names that root")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			REQUIRE(PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report).has_value());
			const Entity nested = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a)));
			REQUIRE(nested.IsValid());

			const auto checkRefused = [rootID](const Error& error)
			{
				CHECK(error.GetCode() == ErrorCode::InvalidArgument);
				CHECK(error.ToString().contains(rootID.ToString()));
			};
			const Status updated = PrefabInstantiator::UpdateInstance(scene, nested, block, PrefabOptions{}, report);
			REQUIRE_FALSE(updated.has_value());
			checkRefused(updated.error());
			const Status reverted = PrefabInstantiator::Revert(scene, nested, block, PrefabOptions{}, report);
			REQUIRE_FALSE(reverted.has_value());
			checkRefused(reverted.error());
			const Status refreshed = PrefabInstantiator::RefreshOverrides(nested, block, PrefabOptions{});
			REQUIRE_FALSE(refreshed.has_value());
			checkRefused(refreshed.error());
			const Status unpacked = PrefabInstantiator::Unpack(nested);
			REQUIRE_FALSE(unpacked.has_value());
			checkRefused(unpacked.error());
			const Result<std::vector<PrefabOverride>> computed = PrefabInstantiator::ComputeOverrides(nested, block, PrefabOptions{});
			REQUIRE_FALSE(computed.has_value());
			checkRefused(computed.error());
			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(nested, block, PrefabOptions{});
			REQUIRE_FALSE(applied.has_value());
			checkRefused(applied.error());
			CHECK(nested.HasComponent<PrefabInstanceComponent>());

			// An OuterPrefabEntityID that no ancestor instance root derives is a broken chain; an entity outside every
			// instance has no override path.
			const Entity orphan = scene.CreateEntity("Orphan");
			orphan.AddComponent<PrefabInstanceComponent>();
			orphan.AddComponent<PrefabLinkComponent>(PrefabLinkComponent{ UUID(0x00000000000b0001), orphan.GetUUID(), UUID(0x99) });
			const Result<std::vector<Entity>> levels = PrefabInstantiator::GetInstanceLevels(orphan);
			REQUIRE_FALSE(levels.has_value());
			CHECK(levels.error().GetCode() == ErrorCode::InvalidState);
			const Result<std::vector<UUID>> path = PrefabInstantiator::GetOverridePath(scene.CreateEntity("Plain"));
			REQUIRE_FALSE(path.has_value());
			CHECK(path.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Decision 8: unpacking an outermost instance unpacks one level. Its own links go, and each instance nested directly
		// in it becomes outermost, taking over the overrides the outermost root recorded within it, re-based onto it.
		TEST_CASE("PrefabInstantiator: unpack makes the instances nested in the instance outermost with the overrides recorded for them")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a));
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002));
			scene.FindEntityByID(visualID).Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, shelf, PrefabOptions{}).has_value());

			REQUIRE(PrefabInstantiator::Unpack(*root).has_value());
			CHECK_FALSE(root->HasComponent<PrefabInstanceComponent>());
			CHECK_FALSE(root->HasComponent<PrefabLinkComponent>());
			const Entity nested = scene.FindEntityByID(nestedID);
			REQUIRE(nested.IsValid());
			CHECK_FALSE(nested.GetComponent<PrefabLinkComponent>().OuterPrefabEntityID.IsValid());
			const std::vector<PrefabOverride>& moved = nested.GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(moved.size() == 1);
			CHECK(moved[0].Path == std::vector<UUID>{ UUID(0x00000000000b0002) });
			CHECK(moved[0].Field == "Visible");
			CHECK(scene.FindEntityByID(visualID).GetComponent<PrefabLinkComponent>().InstanceRoot == nestedID);
			// It is an instance of its own now: an update from its prefab keeps the moved override.
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, nested, block, PrefabOptions{}, report).has_value());
			CHECK_FALSE(scene.FindEntityByID(visualID).GetComponent<MeshRendererComponent>().Visible);
		}

		// Decision 9: prefab.apply at a nested level writes the overrides within that instance into its prefab, re-based onto
		// it; the level root's Name and Transform are its placement and never reach its prefab, its other overrides do. At the
		// outermost level, an edit of a nested instance becomes that instance's own override in the document nesting it.
		TEST_CASE("PrefabInstantiator: apply at a level writes the overrides within it into the level's prefab, the level root's placement left out")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity nested = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a)));
			const Entity visual = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(nested.GetUUID(), UUID(0x00000000000b0002)));
			REQUIRE(nested.IsValid());
			REQUIRE(visual.IsValid());
			nested.SetName("Lantern");
			nested.AddTag("Lit");
			nested.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(5.0f, 0.0f, 0.0f);
			});
			visual.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, shelf, PrefabOptions{}).has_value());
			REQUIRE(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 4);

			const Result<std::vector<Entity>> levels = PrefabInstantiator::GetInstanceLevels(nested);
			REQUIRE(levels.has_value());
			CHECK(*levels == std::vector<Entity>{ *root, nested });

			// At the Block level: the tag and the Visual's edit, never the name or the move.
			const Result<Prefab> atBlock = PrefabInstantiator::ApplyOverrides(nested, nested, block, PrefabOptions{});
			REQUIRE_MESSAGE(atBlock.has_value(), (atBlock ? std::string() : atBlock.error().ToString()));
			const Json* blockRoot = atBlock->FindEntity(UUID(0x00000000000b0001));
			const Json* blockVisual = atBlock->FindEntity(UUID(0x00000000000b0002));
			REQUIRE(blockRoot != nullptr);
			REQUIRE(blockVisual != nullptr);
			CHECK((*blockRoot)["Tags"] == Json::array({ "Lit" }));
			CHECK((*blockRoot)["Name"] == Json("Block"));
			CHECK((*blockRoot)["Components"]["Transform"]["Translation"] == Json::array({ 0, 0, 0 }));
			CHECK((*blockVisual)["Components"]["MeshRenderer"]["Visible"] == Json(false));
			CHECK((*blockVisual)["Components"]["MeshRenderer"]["ReceiveShadows"] == Json(true));

			// At the Shelf level for the nested instance: written into the shelf's document, where the nested root's own record
			// gains the tag and the Visual's edit beside the record it had; its name and move are the shelf's own data.
			const Result<Prefab> atShelf = PrefabInstantiator::ApplyOverrides(nested, *root, shelf, PrefabOptions{});
			REQUIRE_MESSAGE(atShelf.has_value(), (atShelf ? std::string() : atShelf.error().ToString()));
			const Json* shelfNested = atShelf->FindEntity(UUID(0x7a7a));
			REQUIRE(shelfNested != nullptr);
			CHECK((*shelfNested)["Name"] == Json("Lantern"));
			CHECK((*shelfNested)["Components"]["Transform"]["Translation"] == Json::array({ 5, 0, 0 }));
			const Json record = (*shelfNested)["Components"]["Prefab"]["Overrides"];
			REQUIRE(record.size() == 3);
			CHECK(record[0]["Path"] == Json::array({ UUID(0x00000000000b0001).ToString() }));
			CHECK(record[0]["Field"] == Json("Tags"));
			CHECK(record[1]["Field"] == Json("ReceiveShadows"));
			CHECK(record[2]["Field"] == Json("Visible"));
			// The applied shelf instantiates to the same state: the record reproduces the edits.
			const Scope<Scene> check = setup.CreateEmptyScene();
			const Result<Entity> reloaded = PrefabInstantiator::Instantiate(*check, *atShelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(reloaded.has_value());
			const ConstEntity reloadedVisual = std::as_const(*check).FindEntityByID(visual.GetUUID());
			REQUIRE(reloadedVisual.IsValid());
			CHECK_FALSE(reloadedVisual.GetComponent<MeshRendererComponent>().Visible);

			// A level that is not one of the instance's is refused.
			const Result<Prefab> wrong = PrefabInstantiator::ApplyOverrides(nested, visual, block, PrefabOptions{});
			REQUIRE_FALSE(wrong.has_value());
			CHECK(wrong.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Decision 9 through the two-argument apply: writing the instance's state into its prefab keeps the nested instance
		// live, its record composed with the instance's edits of it, so a later rebuild from the nested prefab keeps them.
		TEST_CASE("PrefabInstantiator: apply records the edit of a nested instance as its own override in the prefab nesting it")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a));
			scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002))).Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.CastShadows = false;
			});

			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, shelf, PrefabOptions{});
			REQUIRE_MESSAGE(applied.has_value(), (applied ? std::string() : applied.error().ToString()));
			const Json instance = GetDocumentInstance(*applied, UUID(0x7a7a));
			REQUIRE(instance["Overrides"].size() == 2);
			CHECK(instance["Overrides"][0]["Field"] == Json("CastShadows"));
			CHECK(instance["Overrides"][1]["Field"] == Json("ReceiveShadows"));
			const Json* nested = applied->FindEntity(UUID(0x7a7a));
			REQUIRE(nested != nullptr);
			CHECK((*nested)["Components"]["PrefabLink"]["InstanceRoot"] == Json(UUID(0x7a7a).ToString()));
			CHECK((*nested)["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());

			// The nested prefab gets a new version: the rewritten document keeps both edits.
			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), block);
			const std::map<AssetHandle, Prefab> versions{ { AssetHandle(0x3c9f2e7a11d04b88), edited } };
			const Result<Prefab> rewritten = UpdateNestedPrefabInstances(*applied, setup.GetRegistry(), versions, PrefabOptions{}, report);
			REQUIRE(rewritten.has_value());
			const Json* visual = rewritten->FindEntity(PrefabInstantiator::DeriveInstanceID(UUID(0x7a7a), UUID(0x00000000000b0002)));
			REQUIRE(visual != nullptr);
			CHECK((*visual)["Components"]["MeshRenderer"]["Mesh"] == Json("0000000000000105"));
			CHECK((*visual)["Components"]["MeshRenderer"]["ReceiveShadows"] == Json(false));
			CHECK((*visual)["Components"]["MeshRenderer"]["CastShadows"] == Json(false));
		}

		// Decision 8: a prefab created from an instance keeps the instances nested in it live, outermost in the new document.
		// Prefab::CreateFromEntity moves onto each the overrides its enclosing instance recorded for it; the document nesting
		// it in the enclosing prefab may record more (here the shelf's record of the Visual's shadows), which only its own
		// prefab can tell, so the editor recomputes each record against its prefab (RefreshNestedPrefabOverrides).
		TEST_CASE("PrefabInstantiator: a prefab created from an instance moves the nested instance's overrides, and a refresh recomputes them")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID nestedID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a));
			scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002))).Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, shelf, PrefabOptions{}).has_value());

			const Result<Prefab> created = Prefab::CreateFromEntity(*root, "Case");
			REQUIRE_MESSAGE(created.has_value(), (created ? std::string() : created.error().ToString()));
			const Json* createdRoot = created->FindEntity(rootID);
			REQUIRE(createdRoot != nullptr);
			CHECK_FALSE((*createdRoot)["Components"].contains("Prefab"));
			const Json moved = GetDocumentInstance(*created, nestedID);
			REQUIRE(moved["Overrides"].size() == 1);
			CHECK(moved["Overrides"][0]["Path"] == Json::array({ UUID(0x00000000000b0002).ToString() }));
			CHECK(moved["Overrides"][0]["Field"] == Json("Visible"));
			const Json* createdNested = created->FindEntity(nestedID);
			REQUIRE(createdNested != nullptr);
			CHECK((*createdNested)["Components"]["PrefabLink"]["OuterPrefabEntityID"].is_null());

			const std::map<AssetHandle, Prefab> versions{ { AssetHandle(0x3c9f2e7a11d04b88), block } };
			const Result<Prefab> refreshed = RefreshNestedPrefabOverrides(*created, setup.GetRegistry(), versions, PrefabOptions{});
			REQUIRE_MESSAGE(refreshed.has_value(), (refreshed ? std::string() : refreshed.error().ToString()));
			const Json record = GetDocumentInstance(*refreshed, nestedID);
			REQUIRE(record["Overrides"].size() == 2);
			CHECK(record["Overrides"][0]["Field"] == Json("ReceiveShadows"));
			CHECK(record["Overrides"][1]["Field"] == Json("Visible"));
			// The entities stay as they were.
			const Json* refreshedVisual = refreshed->FindEntity(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002)));
			const Json* createdVisual = created->FindEntity(PrefabInstantiator::DeriveInstanceID(nestedID, UUID(0x00000000000b0002)));
			REQUIRE(refreshedVisual != nullptr);
			REQUIRE(createdVisual != nullptr);
			CHECK(*refreshedVisual == *createdVisual);
		}

		// Decisions 7 and 8: a member of a nested instance deleted from the outermost instance is recorded by its path and
		// stays deleted across updates; revert brings it back.
		TEST_CASE("PrefabInstantiator: a deleted member of a nested instance is recorded by its path and stays deleted")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			const Prefab shelf = MakeShelfPrefab(setup, block);
			LoadReport report;
			const UUID rootID(0x7b7b);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, shelf, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID visualID = PrefabInstantiator::DeriveInstanceID(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x7a7a)), UUID(0x00000000000b0002));
			scene.DestroyEntity(scene.FindEntityByID(visualID));
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, shelf, PrefabOptions{}).has_value());
			const std::vector<PrefabOverride>& recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0].Kind == PrefabOverrideKind::RemoveEntity);
			CHECK(recorded[0].Path == std::vector<UUID>{ UUID(0x7a7a), UUID(0x00000000000b0002) });

			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, shelf, PrefabOptions{}, report).has_value());
			CHECK_FALSE(scene.FindEntityByID(visualID).IsValid());
			REQUIRE(PrefabInstantiator::Revert(scene, *root, shelf, PrefabOptions{}, report).has_value());
			CHECK(scene.FindEntityByID(visualID).IsValid());
		}

		// Docs/Decisions/0031-m20-contract.md decision 7 (E-PFB-DEL; Roadmap M20 acceptance "A deleted prefab member stays
		// deleted after an update"): no override recorded a deleted member, so the next update rebuilt it. The diff records
		// one RemoveEntity for the top of the removed subtree; updates build neither it nor its descendants; revert brings
		// them back with their derived ids.
		TEST_CASE("PrefabInstantiator: a deleted prefab member stays deleted after an update")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab chain = ParsePrefab(setup.GetRegistry(), ChainPrefab);
			LoadReport report;
			const UUID rootID(0x4e4e);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, chain, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID aID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0002));
			const UUID bID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0003));

			// Deleting A (B goes with it) in the edit scene, then the commit's refresh.
			scene.DestroyEntity(scene.FindEntityByID(aID));
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, chain, PrefabOptions{}).has_value());
			const Result<Json> rootJson = SceneSerializer::EntityToJson(*root);
			REQUIRE(rootJson.has_value());
			const Json expected = Json::array({ Json{ { "Path", Json::array({ UUID(0x00000000000f0002).ToString() }) },
				{ "Kind", "RemoveEntity" },
				{ "Component", "" },
				{ "Field", "" },
				{ "Value", nullptr } } });
			CHECK((*rootJson)["Components"]["Prefab"]["Overrides"] == expected);

			// An update from the same prefab, and from one with an entity added under A's place, keeps A and B deleted.
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, chain, PrefabOptions{}, report).has_value());
			CHECK_FALSE(scene.FindEntityByID(aID).IsValid());
			CHECK_FALSE(scene.FindEntityByID(bID).IsValid());
			Result<Json> document = chain.ToJson();
			REQUIRE(document.has_value());
			Json extra = (*document)["Entities"][1];
			extra["ID"] = "00000000000f0004";
			extra["Name"] = "C";
			extra["Parent"] = "00000000000f0001";
			(*document)["Entities"].push_back(extra);
			Result<Prefab> grown = Prefab::FromJson(*document, setup.GetRegistry(), LoadOptions{}, report);
			REQUIRE(grown.has_value());
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *grown, PrefabOptions{}, report).has_value());
			CHECK_FALSE(scene.FindEntityByID(aID).IsValid());
			CHECK_FALSE(scene.FindEntityByID(bID).IsValid());
			CHECK(scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000f0004))).IsValid());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.size() == 1);

			// Revert clears the override, and A and B come back with their derived ids.
			REQUIRE(PrefabInstantiator::Revert(scene, *root, *grown, PrefabOptions{}, report).has_value());
			CHECK(scene.FindEntityByID(aID).IsValid());
			CHECK(scene.FindEntityByID(bID).IsValid());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 7: a RemoveEntity applies to an entity the prefab still has below its
		// root, and an override of an entity it removes has nothing to change; each is dropped with a warning instead, as a
		// stale override is.
		TEST_CASE("PrefabInstantiator: RemoveEntity overrides that no longer apply and overrides of removed entities are dropped with a warning")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab chain = ParsePrefab(setup.GetRegistry(), ChainPrefab);
			LoadReport report;
			const UUID rootID(0x4f4f);
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, chain, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const UUID a(0x00000000000f0002);
			const UUID b(0x00000000000f0003);
			root->Patch<PrefabInstanceComponent>([a, b](PrefabInstanceComponent& instance)
			{
				instance.Overrides = {
					MakeOverride(a, PrefabOverrideKind::RemoveEntity, "", "", Json()),
					MakeOverride(b, PrefabOverrideKind::RemoveEntity, "", "", Json()),
					MakeOverride(b, PrefabOverrideKind::Field, "Transform", "Scale", Json::array({ 2, 2, 2 })),
					MakeOverride(UUID(0x00000000000f0001), PrefabOverrideKind::RemoveEntity, "", "", Json()),
					MakeOverride(UUID(0x00000000000f0009), PrefabOverrideKind::RemoveEntity, "", "", Json()),
				};
			});

			// Only A's RemoveEntity applies: B's is below it, the root cannot be removed, and the prefab has no f0009.
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, chain, PrefabOptions{}, updateReport).has_value());
			CHECK(CountDiagnostics(updateReport, PrefabStaleOverrideCode) == 4);
			const std::vector<PrefabOverride>& kept = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(kept.size() == 1);
			CHECK(kept[0].Path == std::vector<UUID>{ a });
			CHECK(kept[0].Kind == PrefabOverrideKind::RemoveEntity);
			CHECK_FALSE(scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(rootID, a)).IsValid());
			CHECK_FALSE(scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(rootID, b)).IsValid());
			CHECK(root->IsValid());
			CHECK(scene.GetEntityCount() == 1);

			// The prefab without A: its RemoveEntity is stale too, and nothing is left to remove.
			const Prefab rootOnly = RemovePrefabEntities(setup.GetRegistry(), chain, { 2, 1 });
			LoadReport staleReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, rootOnly, PrefabOptions{}, staleReport).has_value());
			CHECK(CountDiagnostics(staleReport, PrefabStaleOverrideCode) == 1);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
		}

		// Docs/Decisions/0031-m20-contract.md decision 7: prefab.apply of an instance with a deleted member writes a prefab
		// without it and its descendants. A reference to the deleted member keeps the prefab-local ID the prefab had, which
		// names no entity any more; an entity that took the member's ID since is outside the instance and is kept as it is.
		TEST_CASE("PrefabInstantiator: apply leaves a deleted member out and keeps a reference to it in prefab space")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;
			const Prefab turret = ParsePrefab(setup.GetRegistry(), TurretPrefab, &schemas);
			LoadReport report;
			const UUID rootID(0x7c7c);
			const UUID barrelID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000d0002));
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, turret, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());

			// The barrel, which the root's Goal names, is deleted.
			scene.DestroyEntity(scene.FindEntityByID(barrelID));
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, turret, options).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0].Path == std::vector<UUID>{ UUID(0x00000000000d0002) });
			CHECK(recorded[0].Kind == PrefabOverrideKind::RemoveEntity);

			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, turret, options);
			REQUIRE(applied.has_value());
			CHECK(applied->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000d0001) });
			const Json* appliedRoot = applied->FindEntity(UUID(0x00000000000d0001));
			REQUIRE(appliedRoot != nullptr);
			CHECK((*appliedRoot)["Components"]["Script"]["Scripts"][0]["Fields"]["Goal"] == "00000000000d0002");

			// Reverting to the applied prefab leaves nothing to override.
			REQUIRE(PrefabInstantiator::Revert(scene, *root, *applied, options, report).has_value());
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
			CHECK(root->GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == Json("00000000000d0002"));
			const Result<std::vector<PrefabOverride>> remaining = PrefabInstantiator::ComputeOverrides(*root, *applied, options);
			REQUIRE(remaining.has_value());
			CHECK(remaining->empty());

			// A user child holding the deleted barrel's ID, as prefab.create {replaceWithInstance} leaves one, is named as it is.
			const Result<Entity> second = PrefabInstantiator::Instantiate(scene, turret, MakeOptions(UUID(0x7d7d)), options, report);
			REQUIRE(second.has_value());
			const UUID secondBarrelID = PrefabInstantiator::DeriveInstanceID(UUID(0x7d7d), UUID(0x00000000000d0002));
			scene.DestroyEntity(scene.FindEntityByID(secondBarrelID));
			static_cast<void>(scene.CreateEntityWithID(secondBarrelID, "Replacement", *second));
			const Result<Prefab> kept = PrefabInstantiator::ApplyOverrides(*second, turret, options);
			REQUIRE(kept.has_value());
			CHECK(kept->GetEntityIDs() == std::vector<UUID>{ UUID(0x00000000000d0001) });
			const Json* keptRoot = kept->FindEntity(UUID(0x00000000000d0001));
			REQUIRE(keptRoot != nullptr);
			CHECK((*keptRoot)["Components"]["Script"]["Scripts"][0]["Fields"]["Goal"] == Json(secondBarrelID.ToString()));
		}

		TEST_CASE("PrefabInstantiator: updates and unpack leave an instance placed inside another alone")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab block = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const UUID outerID(0x8a8a);
			const UUID innerID(0x8b8b);
			const Result<Entity> outer = PrefabInstantiator::Instantiate(scene, block, MakeOptions(outerID), PrefabOptions{}, report);
			REQUIRE(outer.has_value());
			const Entity outerVisual = scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(outerID, UUID(0x00000000000b0002)));
			PrefabInstantiateOptions innerOptions = MakeOptions(innerID);
			innerOptions.Parent = outerVisual;
			const Result<Entity> inner = PrefabInstantiator::Instantiate(scene, block, innerOptions, PrefabOptions{}, report);
			REQUIRE(inner.has_value());
			const UUID innerVisualID = PrefabInstantiator::DeriveInstanceID(innerID, UUID(0x00000000000b0002));

			// To the outer instance, the inner one is a user child: an update of the outer instance keeps it as it is.
			const Prefab edited = EditBlockPrefab(setup.GetRegistry(), block);
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *outer, edited, PrefabOptions{}, report).has_value());
			REQUIRE(inner->IsValid());
			CHECK(inner->GetParent() == outerVisual);
			CHECK(inner->GetComponent<PrefabLinkComponent>().InstanceRoot == innerID);
			const Entity innerVisual = scene.FindEntityByID(innerVisualID);
			REQUIRE(innerVisual.IsValid());
			CHECK(innerVisual.GetComponent<MeshRendererComponent>().Mesh.GetHandle() == AssetHandle(0x0000000000000101));
			CHECK_FALSE(scene.FindEntityByID(PrefabInstantiator::DeriveInstanceID(innerID, UUID(0x00000000000b0003))).IsValid());
			const Result<std::vector<PrefabOverride>> outerOverrides = PrefabInstantiator::ComputeOverrides(*outer, edited, PrefabOptions{});
			REQUIRE(outerOverrides.has_value());
			CHECK(outerOverrides->empty());

			// Unpacking the outer instance removes its own links only.
			REQUIRE(PrefabInstantiator::Unpack(*outer).has_value());
			CHECK_FALSE(outerVisual.HasComponent<PrefabLinkComponent>());
			CHECK(inner->HasComponent<PrefabInstanceComponent>());
			CHECK(inner->GetComponent<PrefabLinkComponent>().InstanceRoot == innerID);
			CHECK(innerVisual.GetComponent<PrefabLinkComponent>().InstanceRoot == innerID);
			const Result<std::vector<PrefabOverride>> innerOverrides = PrefabInstantiator::ComputeOverrides(*inner, block, PrefabOptions{});
			REQUIRE(innerOverrides.has_value());
			CHECK(innerOverrides->empty());
		}

		TEST_CASE("PrefabInstantiator: operations need an instance root of the scene and a prefab")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = LoadBlockPrefab(setup.GetRegistry());
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(UUID(0x9a9a)), PrefabOptions{}, report);
			REQUIRE(root.has_value());
			const Entity member = scene.FindEntityByID(root->GetChildren()[0]);
			const Entity plain = scene.CreateEntity("Plain");
			const uint64_t revision = scene.GetRevision();

			for (const Entity entity : { member, plain, Entity() })
			{
				CAPTURE(entity.IsValid());
				CHECK(GetErrorCode(PrefabInstantiator::UpdateInstance(scene, entity, prefab, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
				CHECK(GetErrorCode(PrefabInstantiator::Revert(scene, entity, prefab, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
				CHECK(GetErrorCode(PrefabInstantiator::ComputeOverrides(entity, prefab, PrefabOptions{})) == ErrorCode::InvalidArgument);
				CHECK(GetErrorCode(PrefabInstantiator::RefreshOverrides(entity, prefab, PrefabOptions{})) == ErrorCode::InvalidArgument);
				CHECK(GetErrorCode(PrefabInstantiator::ApplyOverrides(entity, prefab, PrefabOptions{})) == ErrorCode::InvalidArgument);
				CHECK(GetErrorCode(PrefabInstantiator::Unpack(entity)) == ErrorCode::InvalidArgument);
			}

			const Prefab empty;
			CHECK(GetErrorCode(PrefabInstantiator::UpdateInstance(scene, *root, empty, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::Revert(scene, *root, empty, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::ComputeOverrides(*root, empty, PrefabOptions{})) == ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::ApplyOverrides(*root, empty, PrefabOptions{})) == ErrorCode::InvalidArgument);

			// The root or the parent belongs to another scene.
			const Scope<Scene> other = setup.CreateEmptyScene();
			CHECK(GetErrorCode(PrefabInstantiator::UpdateInstance(*other, *root, prefab, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
			CHECK(GetErrorCode(PrefabInstantiator::Revert(*other, *root, prefab, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
			PrefabInstantiateOptions foreign = MakeOptions(UUID(0x9b9b));
			foreign.Parent = plain;
			CHECK(GetErrorCode(PrefabInstantiator::Instantiate(*other, prefab, foreign, PrefabOptions{}, report)) == ErrorCode::InvalidArgument);
			CHECK(other->GetEntityCount() == 0);
			CHECK(scene.GetRevision() == revision);
			CHECK(root->HasComponent<PrefabInstanceComponent>());
		}

		TEST_CASE("PrefabInstantiator: a reference to an entity outside the instance that shares a prefab-local ID is rejected")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;

			// The prefab is created from entities that stay in the scene, so its prefab-local IDs are their IDs.
			const Entity turret = scene.CreateEntity("Turret");
			const Entity barrel = scene.CreateEntity("Barrel", turret);
			ScriptSlot aim;
			aim.Script = TypedAssetHandle<AssetType::Script>(AssetHandle(Test::FixtureSchemaSource::DefaultOwner.GetValue()));
			aim.Fields["Goal"] = VariantValue(Json(barrel.GetUUID().ToString()));
			static_cast<void>(turret.AddComponent<ScriptComponent>(ScriptComponent{ { aim } }));
			const Result<Prefab> prefab = Prefab::CreateFromEntity(turret, "Turret");
			REQUIRE(prefab.has_value());

			const UUID rootID(0x8c8c);
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, *prefab, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());
			const UUID memberBarrel = PrefabInstantiator::DeriveInstanceID(rootID, barrel.GetUUID());
			CHECK(root->GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == Json(memberBarrel.ToString()));

			// Aiming the instance at the original barrel: an override would store its ID, which is also the prefab-local ID of
			// the instance's own barrel, so the next update would aim at the member instead. Nothing is recorded or applied.
			const auto aimAt = [&root](UUID target)
			{
				root->Patch<ScriptComponent>([target](ScriptComponent& script)
				{
					script.Scripts.front().Fields["Goal"] = VariantValue(Json(target.ToString()));
				});
			};
			aimAt(barrel.GetUUID());
			const Result<std::vector<PrefabOverride>> ambiguous = PrefabInstantiator::ComputeOverrides(*root, *prefab, options);
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidState);
			CHECK(ambiguous.error().GetLocation().Entity == rootID);
			CHECK(GetErrorCode(PrefabInstantiator::RefreshOverrides(*root, *prefab, options)) == ErrorCode::InvalidState);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
			CHECK(GetErrorCode(PrefabInstantiator::ApplyOverrides(*root, *prefab, options)) == ErrorCode::InvalidState);

			// Any other entity outside the instance is an ordinary external reference, kept through updates.
			const Entity other = scene.CreateEntity("Other");
			aimAt(other.GetUUID());
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, *prefab, options).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0].Value.Get() == Json{ { "Goal", other.GetUUID().ToString() } });
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *prefab, options, report).has_value());
			CHECK(root->GetComponent<ScriptComponent>().Scripts.front().Fields.at("Goal").Get() == Json(other.GetUUID().ToString()));
			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, *prefab, options);
			REQUIRE(applied.has_value());
			const Json* appliedRoot = applied->FindEntity(turret.GetUUID());
			REQUIRE(appliedRoot != nullptr);
			CHECK((*appliedRoot)["Components"]["Script"]["Scripts"][0]["Fields"]["Goal"] == Json(other.GetUUID().ToString()));
		}

		TEST_CASE("PrefabInstantiator: unknown components keep the prefab's data and version through instantiation, updates and apply")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Prefab prefab = ParsePrefab(setup.GetRegistry(), HologramPrefab);
			const UUID rootID(0x4e4e);
			const UUID beamID = PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000e0002));
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(rootID), PrefabOptions{}, report);
			REQUIRE(root.has_value());

			// Instantiated into an empty scene: the members carry the prefab's version, and so does the saved scene.
			for (const UUID id : { rootID, beamID })
			{
				const UnknownComponentData* hologram = FindUnknown(scene.FindEntityByID(id), "Hologram");
				REQUIRE(hologram != nullptr);
				CHECK(hologram->Version == 3);
			}
			CHECK(FindUnknown(*root, "Hologram")->Data.Get() == Json{ { "Flicker", 0.5 } });
			const Result<Json> saved = SceneSerializer::ToJson(scene);
			REQUIRE(saved.has_value());
			CHECK((*saved)["ComponentVersions"]["Hologram"] == 3);
			const Result<std::vector<PrefabOverride>> fresh = PrefabInstantiator::ComputeOverrides(*root, prefab, PrefabOptions{});
			REQUIRE(fresh.has_value());
			CHECK(fresh->empty());

			// The prefab moves the component to version 4 with new data: the members follow, and nothing is an override.
			Result<Json> document = prefab.ToJson();
			REQUIRE(document.has_value());
			(*document)["ComponentVersions"]["Hologram"] = 4;
			(*document)["Entities"][1]["Components"]["Hologram"]["Flicker"] = 0.75;
			LoadReport editReport;
			const Result<Prefab> edited = Prefab::FromJson(*document, setup.GetRegistry(), LoadOptions{}, editReport);
			REQUIRE(edited.has_value());
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *edited, PrefabOptions{}, updateReport).has_value());
			CHECK(CountDiagnostics(updateReport, PrefabStaleOverrideCode) == 0);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
			const UnknownComponentData* beam = FindUnknown(scene.FindEntityByID(beamID), "Hologram");
			REQUIRE(beam != nullptr);
			CHECK(beam->Version == 4);
			CHECK(beam->Data.Get() == Json{ { "Flicker", 0.75 } });
			CHECK(FindUnknown(*root, "Hologram")->Version == 4);
			const Result<std::vector<PrefabOverride>> updated = PrefabInstantiator::ComputeOverrides(*root, *edited, PrefabOptions{});
			REQUIRE(updated.has_value());
			CHECK(updated->empty());

			// prefab.apply keeps the data and the version.
			const Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, *edited, PrefabOptions{});
			REQUIRE(applied.has_value());
			const Result<Json> appliedDocument = applied->ToJson();
			REQUIRE(appliedDocument.has_value());
			CHECK((*appliedDocument)["ComponentVersions"]["Hologram"] == 4);
			const Json* appliedBeam = applied->FindEntity(UUID(0x00000000000e0002));
			REQUIRE(appliedBeam != nullptr);
			CHECK((*appliedBeam)["Components"]["Hologram"] == Json{ { "Flicker", 0.75 } });

			// A scene file that lists no version for the component (version 0 on the members) applies with the prefab's.
			Result<Json> unversioned = SceneSerializer::ToJson(scene);
			REQUIRE(unversioned.has_value());
			(*unversioned)["ComponentVersions"].erase("Hologram");
			const Scope<Scene> loaded = setup.CreateEmptyScene();
			LoadReport loadReport;
			REQUIRE(SceneSerializer::FromJson(*loaded, *unversioned, LoadOptions{}, loadReport).has_value());
			REQUIRE(FindUnknown(loaded->FindEntityByID(beamID), "Hologram") != nullptr);
			CHECK(FindUnknown(loaded->FindEntityByID(beamID), "Hologram")->Version == 0);
			const Result<Prefab> reapplied = PrefabInstantiator::ApplyOverrides(loaded->FindEntityByID(rootID), *edited, PrefabOptions{});
			REQUIRE(reapplied.has_value());
			const Result<Json> reappliedDocument = reapplied->ToJson();
			REQUIRE(reappliedDocument.has_value());
			CHECK((*reappliedDocument)["ComponentVersions"]["Hologram"] == 4);
		}

		TEST_CASE("PrefabInstantiator: a script field override leaves the prefab's other script fields to the prefab")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;
			Result<Json> document = JsonReader::Parse(TurretPrefab);
			REQUIRE(document.has_value());
			Json& fields = (*document)["Entities"][0]["Components"]["Script"]["Scripts"][0]["Fields"];
			fields["Torque"] = 10;
			fields["Label"] = "Turret";
			LoadOptions loadOptions;
			loadOptions.Schemas = &schemas;
			LoadReport report;
			const Result<Prefab> prefab = Prefab::FromJson(*document, setup.GetRegistry(), loadOptions, report);
			REQUIRE(prefab.has_value());

			const UUID rootID(0x5c5c);
			const Json barrel(PrefabInstantiator::DeriveInstanceID(rootID, UUID(0x00000000000d0002)).ToString());
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, *prefab, MakeOptions(rootID), options, report);
			REQUIRE(root.has_value());

			// The instance changes Torque and clears Label: the override names those keys only (Label as null, RFC 7386).
			root->Patch<ScriptComponent>([](ScriptComponent& script)
			{
				script.Scripts.front().Fields["Torque"] = VariantValue(Json(50));
				script.Scripts.front().Fields.erase("Label");
			});
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, *prefab, options).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0].Kind == PrefabOverrideKind::Field);
			CHECK(recorded[0].Component == "Script");
			CHECK(recorded[0].Field == "Scripts/0/Fields");
			CHECK(recorded[0].Value.Get() == Json{ { "Label", nullptr }, { "Torque", 50 } });

			// The prefab edits Torque and adds Count: the instance keeps its Torque, gains Count and still has no Label.
			fields["Torque"] = 20;
			fields["Count"] = 3;
			const Result<Prefab> edited = Prefab::FromJson(*document, setup.GetRegistry(), loadOptions, report);
			REQUIRE(edited.has_value());
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *edited, options, updateReport).has_value());
			CHECK(CountDiagnostics(updateReport, PrefabStaleOverrideCode) == 0);
			const std::map<std::string, VariantValue>& updated = root->GetComponent<ScriptComponent>().Scripts.front().Fields;
			CHECK(updated.at("Torque").Get() == Json(50));
			REQUIRE(updated.contains("Count"));
			CHECK(updated.at("Count").Get() == Json(3));
			CHECK(updated.at("Goal").Get() == barrel);
			CHECK_FALSE(updated.contains("Label"));
			const Result<std::vector<PrefabOverride>> again = PrefabInstantiator::ComputeOverrides(*root, *edited, options);
			REQUIRE(again.has_value());
			REQUIRE(again->size() == 1);
			CHECK((*again)[0].Value == recorded[0].Value);
		}

		// Docs/Decisions/0031-m20-contract.md decision 19 (S-MULTI per-slot overrides): with as many slots as the prefab
		// entity, an instance's change to one slot is recorded as that slot's own override ("Scripts/<slot>/<member>"), so the
		// prefab's edits of the other slots still reach it; a list of another length is recorded whole; and a slot override
		// whose slot the prefab no longer has is dropped as stale.
		TEST_CASE("PrefabInstantiator: an override of one script slot is recorded and applied per slot")
		{
			Test::SceneTestFixture setup;
			Scene& scene = setup.GetScene();
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;
			const Prefab prefab = ParsePrefab(setup.GetRegistry(), TwoSlotTurretPrefab, &schemas);
			LoadReport report;
			const Result<Entity> root = PrefabInstantiator::Instantiate(scene, prefab, MakeOptions(UUID(0x6d6d)), options, report);
			REQUIRE(root.has_value());

			// The syntax of a slot override's Field.
			const std::optional<ScriptSlotField> parsed = PrefabInstantiator::ParseScriptSlotField("Scripts/1/Fields");
			REQUIRE(parsed.has_value());
			CHECK(parsed->Slot == 1);
			CHECK(parsed->Member == "Fields");
			CHECK(PrefabInstantiator::MakeScriptSlotField(1, "ExecutionOrder") == "Scripts/1/ExecutionOrder");
			for (const std::string_view invalid : { "Scripts", "Scripts/01/Fields", "Scripts/1/Name", "Scripts/-1/Fields", "Scripts/1", "Fields" })
			{
				CAPTURE(invalid);
				CHECK_FALSE(PrefabInstantiator::ParseScriptSlotField(invalid).has_value());
			}

			// The instance changes slot 1's Count: one override of that slot's Fields, its changed key only.
			const Result<Json> twoSlots = JsonReader::Parse(R"({ "Scripts": [
				{ "Script": "00000000c0ffee01", "Fields": { "Torque": 10 }, "ExecutionOrder": 0 },
				{ "Script": "00000000c0ffee02", "Fields": { "Count": 7 }, "ExecutionOrder": 5 } ] })");
			REQUIRE(twoSlots.has_value());
			REQUIRE(ComponentAccess::SetComponentJson(*root, "Script", *twoSlots, &schemas).has_value());
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, prefab, options).has_value());
			const std::vector<PrefabOverride> recorded = root->GetComponent<PrefabInstanceComponent>().Overrides;
			REQUIRE(recorded.size() == 1);
			CHECK(recorded[0].Kind == PrefabOverrideKind::Field);
			CHECK(recorded[0].Component == "Script");
			CHECK(recorded[0].Field == "Scripts/1/Fields");
			CHECK(recorded[0].Value.Get() == Json{ { "Count", 7 } });

			// The prefab edits slot 0: the instance follows there and keeps its own slot 1.
			Result<Json> document = JsonReader::Parse(TwoSlotTurretPrefab);
			REQUIRE(document.has_value());
			(*document)["Entities"][0]["Components"]["Script"]["Scripts"][0]["Fields"]["Torque"] = 20;
			LoadOptions loadOptions;
			loadOptions.Schemas = &schemas;
			const Result<Prefab> edited = Prefab::FromJson(*document, setup.GetRegistry(), loadOptions, report);
			REQUIRE(edited.has_value());
			LoadReport updateReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *edited, options, updateReport).has_value());
			const Result<Json> updated = ComponentAccess::GetComponentJson(*root, "Script");
			REQUIRE(updated.has_value());
			CHECK((*updated)["Scripts"][0]["Fields"]["Torque"] == Json(20));
			CHECK((*updated)["Scripts"][1]["Fields"]["Count"] == Json(7));

			// A third slot on the instance changes the list's length: the whole list is one override.
			Json threeSlots = *updated;
			threeSlots["Scripts"].push_back(Json{ { "Script", "00000000c0ffee03" } });
			REQUIRE(ComponentAccess::SetComponentJson(*root, "Script", threeSlots, &schemas).has_value());
			const Result<std::vector<PrefabOverride>> whole = PrefabInstantiator::ComputeOverrides(*root, *edited, options);
			REQUIRE(whole.has_value());
			REQUIRE(whole->size() == 1);
			CHECK((*whole)[0].Field == "Scripts");

			// A prefab left with one slot makes the slot 1 override stale.
			REQUIRE(ComponentAccess::SetComponentJson(*root, "Script", *updated, &schemas).has_value());
			REQUIRE(PrefabInstantiator::RefreshOverrides(*root, *edited, options).has_value());
			(*document)["Entities"][0]["Components"]["Script"]["Scripts"].erase(1);
			const Result<Prefab> shortened = Prefab::FromJson(*document, setup.GetRegistry(), loadOptions, report);
			REQUIRE(shortened.has_value());
			LoadReport staleReport;
			REQUIRE(PrefabInstantiator::UpdateInstance(scene, *root, *shortened, options, staleReport).has_value());
			CHECK(CountDiagnostics(staleReport, PrefabStaleOverrideCode) == 1);
			CHECK(root->GetComponent<PrefabInstanceComponent>().Overrides.empty());
		}

		// Decisions 9 and 19: applying an instance writes its edits of a nested instance into the nested instance's record in
		// the prefab nesting it, slot by slot: a slot override joins the record's overrides of the other slots, and one of a
		// slot that the record replaces with a whole list is written into that list.
		TEST_CASE("PrefabInstantiator: apply composes a nested instance's script slot edits with its record")
		{
			Test::SceneTestFixture setup;
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			PrefabOptions options;
			options.Schemas = &schemas;
			const Prefab turret = ParsePrefab(setup.GetRegistry(), TwoSlotTurretPrefab, &schemas);
			const auto makeRack = [&setup, &turret, &options](const Json& nestedScript)
			{
				const Scope<Scene> source = setup.CreateEmptyScene();
				const Entity rackRoot = source->CreateEntityWithID(UUID(0x8181), "Rack");
				PrefabInstantiateOptions nestedOptions;
				nestedOptions.PrefabHandle = AssetHandle(0x2510cafe0001);
				nestedOptions.RootID = UUID(0x8282);
				nestedOptions.Parent = rackRoot;
				LoadReport report;
				const Result<Entity> nested = PrefabInstantiator::Instantiate(*source, turret, nestedOptions, options, report);
				REQUIRE(nested.has_value());
				REQUIRE(ComponentAccess::SetComponentJson(*nested, "Script", nestedScript, options.Schemas).has_value());
				REQUIRE(PrefabInstantiator::RefreshOverrides(*nested, turret, options).has_value());
				Result<Prefab> rack = Prefab::CreateFromEntity(rackRoot, "Rack");
				REQUIRE_MESSAGE(rack.has_value(), (rack ? std::string() : rack.error().ToString()));
				return std::move(*rack);
			};
			const auto parse = [](std::string_view text)
			{
				Result<Json> json = JsonReader::Parse(text);
				REQUIRE(json.has_value());
				return std::move(*json);
			};
			// The scene's rack instance changes the nested turret's slot 0 Torque to 20, its other values as the rack has them.
			const auto applyTorque = [&setup, &options](const Prefab& rack, Json script)
			{
				const Scope<Scene> scene = setup.CreateEmptyScene();
				LoadReport report;
				PrefabInstantiateOptions rackOptions;
				rackOptions.PrefabHandle = AssetHandle(0x2510cafe0002);
				rackOptions.RootID = UUID(0x9191);
				const Result<Entity> root = PrefabInstantiator::Instantiate(*scene, rack, rackOptions, options, report);
				REQUIRE(root.has_value());
				const Entity nested = scene->FindEntityByID(PrefabInstantiator::DeriveInstanceID(UUID(0x9191), UUID(0x8282)));
				REQUIRE(nested.IsValid());
				script["Scripts"][0]["Fields"]["Torque"] = 20;
				REQUIRE(ComponentAccess::SetComponentJson(nested, "Script", script, options.Schemas).has_value());
				Result<Prefab> applied = PrefabInstantiator::ApplyOverrides(*root, rack, options);
				REQUIRE_MESSAGE(applied.has_value(), (applied ? std::string() : applied.error().ToString()));
				return GetDocumentInstance(*applied, UUID(0x8282))["Overrides"];
			};

			// The rack records slot 1's Count; the scene's edit of slot 0 joins it as its own slot override.
			const Json counted = parse(R"({ "Scripts": [
				{ "Script": "00000000c0ffee01", "Fields": { "Torque": 10 }, "ExecutionOrder": 0 },
				{ "Script": "00000000c0ffee02", "Fields": { "Count": 7 }, "ExecutionOrder": 5 } ] })");
			const Json slots = applyTorque(makeRack(counted), counted);
			REQUIRE(slots.size() == 2);
			CHECK(slots[0]["Field"] == Json("Scripts/0/Fields"));
			CHECK(slots[0]["Value"] == Json{ { "Torque", 20 } });
			CHECK(slots[1]["Field"] == Json("Scripts/1/Fields"));
			CHECK(slots[1]["Value"] == Json{ { "Count", 7 } });

			// The rack gives the turret a third slot, a whole list; the scene's edit of slot 0 is written into that list.
			Json extended = counted;
			extended["Scripts"].push_back(Json{ { "Script", "00000000c0ffee03" }, { "Fields", Json::object() }, { "ExecutionOrder", 0 } });
			const Json list = applyTorque(makeRack(extended), extended);
			REQUIRE(list.size() == 1);
			CHECK(list[0]["Field"] == Json("Scripts"));
			REQUIRE(list[0]["Value"].size() == 3);
			CHECK(list[0]["Value"][0]["Fields"]["Torque"] == Json(20));
			CHECK(list[0]["Value"][1]["Fields"]["Count"] == Json(7));
			CHECK(list[0]["Value"][2]["Script"] == Json("00000000c0ffee03"));
		}
	}

}
