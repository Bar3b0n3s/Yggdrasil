#include "TestsPCH.h"

#include "Engine/Scene/PrefabAsset.h"

#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/TestData.h"

namespace Engine {

	TEST_SUITE("Scene")
	{
		TEST_CASE("PrefabAsset: instantiating by handle records the prefab asset" * doctest::skip(true))
		{
			Test::AssetTestFixture assets;
			Result<std::string> block = Test::ReadTestDataText("Prefabs/Block.prefab");
			REQUIRE(block.has_value());
			assets.WriteProjectText("Assets/Prefabs/Block.prefab", *block);
			assets.OpenProject(false);
			const AssetHandle handle = assets.GetManager().Resolve("Assets/Prefabs/Block.prefab").value_or(AssetHandle());
			REQUIRE(handle.IsValid());

			LoadReport report;
			Result<Prefab> prefab = LoadPrefabAsset(assets.GetManager(), handle, assets.GetRegistry(), report);
			REQUIRE_MESSAGE(prefab.has_value(), prefab.error().ToString());

			// The scene shares the fixture's registry, so the prefab's components are the scene's.
			UUIDGenerator generator = UUIDGenerator::CreateDeterministic(7);
			SceneSpecification specification;
			specification.Name = "Test";
			specification.Registry = &assets.GetRegistry();
			specification.IdGenerator = &generator;
			Scope<Scene> scene = Scene::Create(specification);
			const UUID root = generator.Next();
			Result<Entity> instance = InstantiatePrefabAsset(*scene, assets.GetManager(),
				{ .PrefabHandle = handle, .RootID = root, .Parent = {}, .SiblingIndex = std::nullopt, .RootTransform = std::nullopt }, { .Schemas = nullptr },
				report);
			REQUIRE_MESSAGE(instance.has_value(), instance.error().ToString());
			CHECK(instance->GetUUID() == root);
			REQUIRE(instance->HasComponent<PrefabInstanceComponent>());
			CHECK(instance->GetComponent<PrefabInstanceComponent>().Prefab.GetHandle() == handle);
		}

		TEST_CASE("PrefabAsset: a handle of another type, an unknown handle and a null handle are errors" * doctest::skip(true))
		{
			Test::AssetTestFixture assets;
			assets.WriteProjectText("Assets/Red.material", R"({"Format": "Material", "Version": 1})");
			assets.OpenProject(false);
			const AssetHandle material = assets.GetManager().Resolve("Assets/Red.material").value_or(AssetHandle());
			LoadReport report;
			Result<Prefab> wrongType = LoadPrefabAsset(assets.GetManager(), material, assets.GetRegistry(), report);
			REQUIRE_FALSE(wrongType.has_value());
			CHECK(wrongType.error().GetCode() == ErrorCode::Validation);
			Result<Prefab> unknown = LoadPrefabAsset(assets.GetManager(), AssetHandle(0x123456789ull), assets.GetRegistry(), report);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::NotFound);
			Result<Prefab> null = LoadPrefabAsset(assets.GetManager(), AssetHandle(), assets.GetRegistry(), report);
			REQUIRE_FALSE(null.has_value());
			CHECK(null.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
