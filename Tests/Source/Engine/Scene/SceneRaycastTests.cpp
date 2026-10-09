#include "TestsPCH.h"

#include "Engine/Scene/SceneRaycast.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <limits>

namespace Engine {
	namespace {

		constexpr AssetHandle QueryMesh{ 1001 };
		Ref<MeshData> MakeQueryMesh()
		{
			auto mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { -1, -1, 0 } }, { .Position = { 1, -1, 0 } }, { .Position = { 0, 1, 0 } } };
			mesh->Indices = { 0, 1, 2 };
			for (const auto& vertex : mesh->Vertices)
				mesh->Bounds.Extend(vertex.Position);
			mesh->Submeshes = { { .IndexCount = 3, .Bounds = mesh->Bounds } };
			mesh->Slots = { { .Name = "Default" } };
			return mesh;
		}
		Entity AddQueryMesh(Scene& scene, UUID id, float z = 0)
		{
			Entity entity = scene.CreateEntityWithID(id, "Triangle");
			entity.AddComponent<MeshRendererComponent>().Mesh.SetHandle(QueryMesh);
			entity.Patch<TransformComponent>([z](TransformComponent& t)
			{
				t.Translation.z = z;
			});
			return entity;
		}
		SceneRaycastRequest ForwardQuery()
		{
			return { .Ray = { { 0, 0, 5 }, { 0, 0, -2 } }, .MaxDistance = 10 };
		}

	}
	TEST_SUITE("Scene")
	{
		TEST_CASE("Raycast: a cold CPU mesh loads synchronously without a graphics device")
		{
			Test::SceneTestFixture fixture;
			Test::AssetTestFixture assetFixture;
			assetFixture.OpenProject(false);
			AssetManager& assets = assetFixture.GetManager();
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(20));
			entity.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			});
			CHECK(assets.GetVersion(BuiltinAssetHandles::CubeMesh) == 0);
			const auto result = RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery());
			REQUIRE((result && *result));
			CHECK((*result)->Entity == entity.GetUUID());
			CHECK((*result)->Distance == 4.5f);
			CHECK(assets.GetVersion(BuiltinAssetHandles::CubeMesh) > 0);
			CHECK(assets.GetDiagnostics().empty());
		}
		TEST_CASE("Raycast: a missing mesh uses the renderer CPU placeholder and preserves its diagnostic")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			const AssetHandle missing(0x1111222233334444ull);
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(20));
			entity.Patch<MeshRendererComponent>([missing](MeshRendererComponent& renderer)
			{
				renderer.Mesh.SetHandle(missing);
			});
			Test::ExpectLog expected(LogLevel::Error, missing.ToString());
			for (int repeat = 0; repeat < 2; ++repeat)
			{
				const auto result = RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery());
				REQUIRE((result && *result));
				CHECK((*result)->Entity == entity.GetUUID());
				CHECK((*result)->Distance == 4.5f);
				REQUIRE(assets.GetDiagnostics().size() == 1);
				CHECK(assets.GetDiagnostics()[0].Code == AssetMissingCode);
				CHECK(assets.GetDiagnostics()[0].Asset == missing);
			}
		}
		TEST_CASE("Raycast: CPU ray hits expected triangle")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(40));
			const uint64_t revision = fixture.GetScene().GetRevision();
			const auto result = RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery());
			REQUIRE((result && *result));
			CHECK((*result)->Entity == entity.GetUUID());
			CHECK((*result)->Distance == 5);
			CHECK((*result)->Position == glm::vec3(0));
			CHECK((*result)->Normal == glm::vec3(0, 0, 1));
			CHECK((*result)->Barycentric == glm::vec3(0.25f, 0.25f, 0.5f));
			CHECK(fixture.GetScene().GetRevision() == revision);
			// Material alpha and face winding do not discard visual query geometry.
			entity.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Scale = { -2, 3, 1 };
			});
			entity.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Materials.resize(1);
				r.Materials[0].SetHandle(AssetHandle(9000));
			});
			auto back = ForwardQuery();
			back.Ray = { { 0, 0, -5 }, { 0, 0, 1 } };
			const auto mirrored = RaycastScene(fixture.GetScene(), assets, {}, back);
			REQUIRE((mirrored && *mirrored));
			CHECK((*mirrored)->Normal == glm::vec3(0, 0, -1));
			CHECK((*mirrored)->Distance == 5);
		}
		TEST_CASE("Raycast: ties use UUID then submesh and triangle")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			auto mesh = MakeQueryMesh();
			mesh->Indices = { 0, 1, 2, 0, 1, 2 };
			mesh->Submeshes[0].IndexCount = 6;
			mesh->Submeshes.push_back(mesh->Submeshes[0]);
			assets.Publish(QueryMesh, mesh);
			static_cast<void>(AddQueryMesh(fixture.GetScene(), UUID(70)));
			static_cast<void>(AddQueryMesh(fixture.GetScene(), UUID(20)));
			for (int repeat = 0; repeat < 3; ++repeat)
			{
				const auto result = RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery());
				REQUIRE((result && *result));
				CHECK((*result)->Entity == UUID(20));
				CHECK((*result)->Submesh == 0);
				CHECK((*result)->Triangle == 0);
			}
		}
		TEST_CASE("Raycast: masks filter visual geometry without requiring a physics world")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			Scene& scene = fixture.GetScene();
			Entity root = scene.CreateEntity("Root");
			root.AddComponent<RigidBodyComponent>().Layer = "World";
			Entity child = AddQueryMesh(scene, UUID(20));
			REQUIRE(scene.SetParent(child, root, {}, false));
			child.AddComponent<CharacterControllerComponent>().Layer = "Character";
			const std::vector<std::string> names{ "Default", "World", "Character" };
			const auto layers = PhysicsLayerTable::Create(names, {});
			REQUIRE(layers);
			auto ray = ForwardQuery();
			ray.LayerMask = 2;
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
			ray.LayerMask = 4;
			CHECK_FALSE(RaycastScene(scene, assets, *layers, ray)->has_value());
			child.AddComponent<RigidBodyComponent>().Layer = "Character";
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
			child.RemoveComponent<RigidBodyComponent>();
			root.RemoveComponent<RigidBodyComponent>();
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
			child.Patch<CharacterControllerComponent>([](CharacterControllerComponent& c)
			{
				c.Layer = "Unknown";
			});
			ray.LayerMask = 1;
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
		}
		TEST_CASE("Raycast: invalid rays fail and a zero mask is an empty hit")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(20));
			auto ray = ForwardQuery();
			ray.LayerMask = 0;
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray)->has_value());
			for (float bad : { 0.0f, -1.0f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				ray = ForwardQuery();
				ray.MaxDistance = bad;
				CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray));
			}
			ray = ForwardQuery();
			ray.Ray.Direction = glm::vec3(0);
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray));
			ray.Ray.Direction.x = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray));
			ray = ForwardQuery();
			ray.Ray.Origin.x = std::numeric_limits<float>::infinity();
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray));
			ray = ForwardQuery();
			ray.Alpha = -0.1f;
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ray));
			entity.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Scale.x = 0;
			});
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
			entity.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Scale.x = 1;
			});
			auto mesh = MakeQueryMesh();
			mesh->Indices = { 0, 0, 0 };
			assets.Publish(QueryMesh, mesh);
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
		}
		TEST_CASE("Raycast: disabled invisible and pending-destruction meshes are excluded")
		{
			Test::SceneTestFixture fixture(1, true);
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(20));
			entity.SetActive(false);
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
			entity.SetActive(true);
			entity.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Visible = false;
			});
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
			entity.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Visible = true;
				r.Mesh = {};
			});
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
			entity.Patch<MeshRendererComponent>([](MeshRendererComponent& r)
			{
				r.Mesh.SetHandle(QueryMesh);
			});
			fixture.GetScene().DestroyEntity(entity);
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, ForwardQuery())->has_value());
		}
		TEST_CASE("Raycast: play queries use the rendered pose at the view alpha")
		{
			Test::SceneTestFixture fixture(1, true);
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			Entity entity = AddQueryMesh(fixture.GetScene(), UUID(20), -4);
			entity.AddComponent<PreviousWorldTransformComponent>();
			TransformSystem::Update(fixture.GetScene());
			auto ray = ForwardQuery();
			ray.Alpha = 0.25f;
			const auto result = RaycastScene(fixture.GetScene(), assets, {}, ray);
			REQUIRE((result && *result));
			CHECK((*result)->Distance == 6);
			CHECK((*result)->Position.z == ComputeRenderedWorldMatrix(entity, ray.Alpha)[3].z);
			entity.AddComponent<InterpolationResetTag>();
			CHECK(RaycastScene(fixture.GetScene(), assets, {}, ray)->value().Distance == 9);
		}
		TEST_CASE("Raycast: an excluded near hit cannot hide an in-interval triangle")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryMesh, MakeQueryMesh());
			static_cast<void>(AddQueryMesh(fixture.GetScene(), UUID(20), 4));
			static_cast<void>(AddQueryMesh(fixture.GetScene(), UUID(30), 2));
			auto ray = ForwardQuery();
			ray.MinDistance = 2;
			ray.MaxDistance = 3;
			const auto result = RaycastScene(fixture.GetScene(), assets, {}, ray);
			REQUIRE((result && *result));
			CHECK((*result)->Entity == UUID(30));
			CHECK((*result)->Distance == 3);
			ray.Ray.Origin.z = 2;
			ray.MinDistance = 0;
			CHECK(RaycastScene(fixture.GetScene(), assets, {}, ray)->value().Distance == 0);
		}
	}

}
