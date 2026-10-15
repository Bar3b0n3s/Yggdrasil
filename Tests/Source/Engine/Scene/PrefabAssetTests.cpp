#include "TestsPCH.h"

#include "Engine/Scene/PrefabAsset.h"

#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/FixtureSchemaSource.h"
#include "Support/TestData.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		// A one-entity prefab whose Script component assigns Test::FixtureSchemaSource::DefaultOwner, the script of the
		// standard fixture schema, and sets its Torque field.
		constexpr std::string_view ScriptedPrefab = R"({
	"Format": "Prefab", "Version": 1, "Name": "Turret", "Root": "00000000000c0001",
	"ComponentVersions": { "Transform": 1, "Script": 1 },
	"Entities": [ { "ID": "00000000000c0001", "Name": "Turret", "Parent": null, "Active": true, "Tags": [],
		"Components": { "Transform": {}, "Script": { "Script": "00000000c0ffee01", "Fields": { "Torque": 50 }, "ExecutionOrder": 0 } } } ]
})";

		AssetHandle WriteScriptedPrefab(Test::AssetTestFixture& assets)
		{
			assets.WriteProjectText("Assets/Prefabs/Turret.prefab", std::string(ScriptedPrefab));
			assets.OpenProject(false);
			return assets.GetManager().Resolve("Assets/Prefabs/Turret.prefab").value_or(AssetHandle());
		}

		Scope<Scene> CreateAssetScene(Test::AssetTestFixture& assets, UUIDGenerator& generator)
		{
			SceneSpecification specification;
			specification.Name = "Test";
			specification.Registry = &assets.GetRegistry();
			specification.IdGenerator = &generator;
			return Scene::Create(specification);
		}

		std::string DescribeDiagnostics(const LoadReport& report)
		{
			std::string text;
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
				text += std::format("{} {}: {}\n", diagnostic.Code, diagnostic.JsonPointer, diagnostic.Message);
			return text;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PrefabAsset: instantiating by handle records the prefab asset")
		{
			Test::AssetTestFixture assets;
			Result<std::string> block = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(block.has_value());
			assets.WriteProjectText("Assets/Prefabs/Block.prefab", *block);
			assets.OpenProject(false);
			const AssetHandle handle = assets.GetManager().Resolve("Assets/Prefabs/Block.prefab").value_or(AssetHandle());
			REQUIRE(handle.IsValid());

			LoadReport report;
			Result<Prefab> prefab = LoadPrefabAsset(assets.GetManager(), handle, assets.GetRegistry(), nullptr, report);
			REQUIRE_MESSAGE(prefab.has_value(), prefab.error().ToString());

			// The scene shares the fixture's registry, so the prefab's components are the scene's.
			UUIDGenerator generator = UUIDGenerator::CreateDeterministic(7);
			Scope<Scene> scene = CreateAssetScene(assets, generator);
			const UUID root = generator.Next();
			Result<Entity> instance = InstantiatePrefabAsset(*scene, assets.GetManager(),
				{ .PrefabHandle = handle, .RootID = root, .Parent = {}, .SiblingIndex = std::nullopt, .RootTransform = std::nullopt }, { .Schemas = nullptr },
				report);
			REQUIRE_MESSAGE(instance.has_value(), instance.error().ToString());
			CHECK(instance->GetUUID() == root);
			REQUIRE(instance->HasComponent<PrefabInstanceComponent>());
			CHECK(instance->GetComponent<PrefabInstanceComponent>().Prefab.GetHandle() == handle);
		}

		TEST_CASE("PrefabAsset: script field values load without warnings when schemas are given")
		{
			Test::AssetTestFixture assets;
			const AssetHandle handle = WriteScriptedPrefab(assets);
			REQUIRE(handle.IsValid());
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();

			LoadReport resolved;
			const Result<Prefab> prefab = LoadPrefabAsset(assets.GetManager(), handle, assets.GetRegistry(), &schemas, resolved);
			REQUIRE_MESSAGE(prefab.has_value(), prefab.error().ToString());
			CHECK_MESSAGE(resolved.Diagnostics.empty(), DescribeDiagnostics(resolved));

			// Without schemas the value cannot be checked: it is kept, with the warning every caller reported before.
			LoadReport unresolved;
			REQUIRE(LoadPrefabAsset(assets.GetManager(), handle, assets.GetRegistry(), nullptr, unresolved).has_value());
			const bool warned = std::ranges::any_of(unresolved.Diagnostics, [](const LoadDiagnostic& diagnostic)
			{
				return diagnostic.Code == VariantUnresolvedCode && diagnostic.JsonPointer == "/Entities/0/Components/Script/Fields/Torque";
			});
			CHECK_MESSAGE(warned, DescribeDiagnostics(unresolved));
		}

		TEST_CASE("PrefabAsset: instantiating a prefab with script field values reports nothing when the options carry schemas")
		{
			Test::AssetTestFixture assets;
			const AssetHandle handle = WriteScriptedPrefab(assets);
			REQUIRE(handle.IsValid());
			const Test::FixtureSchemaSource schemas = Test::FixtureSchemaSource::CreateStandard();
			UUIDGenerator generator = UUIDGenerator::CreateDeterministic(7);
			Scope<Scene> scene = CreateAssetScene(assets, generator);

			// The prefab itself is read with the options' schemas, not only the instance's members, so a valid script field
			// value is no REFLECTION_VARIANT_UNRESOLVED warning per instance.
			LoadReport report;
			const Result<Entity> instance = InstantiatePrefabAsset(*scene, assets.GetManager(),
				{ .PrefabHandle = handle, .RootID = generator.Next(), .Parent = {}, .SiblingIndex = std::nullopt, .RootTransform = std::nullopt },
				{ .Schemas = &schemas }, report);
			REQUIRE_MESSAGE(instance.has_value(), instance.error().ToString());
			CHECK_MESSAGE(report.Diagnostics.empty(), DescribeDiagnostics(report));
			REQUIRE(instance->HasComponent<ScriptComponent>());
			CHECK(instance->GetComponent<ScriptComponent>().Fields.at("Torque").Get() == Json(50));
		}

		TEST_CASE("PrefabAsset: a handle of another type, an unknown handle and a null handle are errors")
		{
			Test::AssetTestFixture assets;
			assets.WriteProjectText("Assets/Red.material", R"({"Format": "Material", "Version": 1})");
			assets.OpenProject(false);
			const AssetHandle material = assets.GetManager().Resolve("Assets/Red.material").value_or(AssetHandle());
			LoadReport report;
			Result<Prefab> wrongType = LoadPrefabAsset(assets.GetManager(), material, assets.GetRegistry(), nullptr, report);
			REQUIRE_FALSE(wrongType.has_value());
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
			Result<Prefab> unknown = LoadPrefabAsset(assets.GetManager(), AssetHandle(0x123456789ull), assets.GetRegistry(), nullptr, report);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			Result<Prefab> null = LoadPrefabAsset(assets.GetManager(), AssetHandle(), assets.GetRegistry(), nullptr, report);
			REQUIRE_FALSE(null.has_value());
			CHECK(null.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
