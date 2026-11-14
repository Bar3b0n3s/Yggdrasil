#include "TestsPCH.h"

#include "Engine/Scene/SceneRaycast.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	constexpr AssetHandle QueryMesh{ 1001 };

	static Ref<MeshData> MakeQueryMesh()
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

	static Entity AddQueryMesh(Scene& scene, UUID id, float z = 0)
	{
		Entity entity = scene.CreateEntityWithID(id, "Triangle");
		entity.AddComponent<MeshRendererComponent>().Mesh.SetHandle(QueryMesh);
		entity.Patch<TransformComponent>([z](TransformComponent& t)
		{
			t.Translation.z = z;
		});
		return entity;
	}

	static SceneRaycastRequest ForwardQuery()
	{
		return { .Ray = { { 0, 0, 5 }, { 0, 0, -2 } }, .MaxDistance = 10 };
	}

	constexpr AssetHandle QueryHeightfield{ 1002 };
	constexpr AssetHandle FlatQueryMesh{ 1003 };

	// A 5 x 5 heightfield whose sample (x, z) is x^2 * 2000 + z^2 * 1500 (bowed along both axes, so each cell's two
	// triangles are different planes), or, `planar`, x * 8000 + z * 4000 (one plane).
	static Ref<HeightfieldData> MakeQueryHeightfield(bool planar)
	{
		auto heightfield = CreateRef<HeightfieldData>(MakeFlatHeightfield(5));
		for (uint32_t z = 0; z < 5; ++z)
		{
			for (uint32_t x = 0; x < 5; ++x)
				heightfield->Heights[x + 5 * z] = static_cast<uint16_t>(planar ? x * 8000 + z * 4000 : x * x * 2000 + z * z * 1500);
		}
		return heightfield;
	}

	// The triangle of MakeQueryMesh laid flat in the XZ plane, facing +Y.
	static Ref<MeshData> MakeFlatQueryMesh()
	{
		auto mesh = MakeQueryMesh();
		mesh->Vertices = { { .Position = { -1, 0, 1 } }, { .Position = { 1, 0, 1 } }, { .Position = { 0, 0, -1 } } };
		mesh->Bounds = {};
		for (const auto& vertex : mesh->Vertices)
			mesh->Bounds.Extend(vertex.Position);
		mesh->Submeshes[0].Bounds = mesh->Bounds;
		return mesh;
	}

	// A Terrain of QueryHeightfield over `size` metres, HeightScale 10, at `translation`.
	static Entity AddQueryTerrain(Scene& scene, UUID id, glm::vec2 size, const glm::vec3& translation)
	{
		Entity entity = scene.CreateEntityWithID(id, "Terrain");
		entity.AddComponent<TerrainComponent>(
			TerrainComponent{ .Heightfield = TypedAssetHandle<AssetType::Heightfield>(QueryHeightfield), .Size = size, .HeightScale = 10.0f });
		entity.Patch<TransformComponent>([&translation](TransformComponent& t)
		{
			t.Translation = translation;
		});
		return entity;
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

		// M26 (W26.2's review; Docs/Decisions/0037-m26-contract.md decisions 21 and 22): a terrain is hit on the triangles the
		// renderer draws and the collider shapes, SampleTerrainHeight's and SampleTerrainNormal's surface, as the GPU click
		// selects it.
		TEST_CASE("Raycast: a ray hits a terrain on its triangle's surface, with that triangle's normal")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			const Ref<HeightfieldData> heightfield = MakeQueryHeightfield(false);
			assets.Publish(QueryHeightfield, heightfield);
			const glm::vec3 translation(1.0f, 2.0f, 3.0f);
			const Entity terrain = AddQueryTerrain(fixture.GetScene(), UUID(50), glm::vec2(8.0f), translation);
			const TerrainExtent extent{ .Size = glm::vec2(8.0f), .HeightScale = 10.0f };
			// Local (3.5, 2.25) lies in cell (1, 1) at fractions (0.75, 0.125): its (x + 1, z) triangle, 2 * (1 * 4 + 1) + 1;
			// local (2.5, 3.5) at fractions (0.25, 0.75): its (x, z + 1) triangle of cell (1, 1), 2 * (1 * 4 + 1).
			for (const auto& [local, triangle] : { std::pair{ glm::vec2(3.5f, 2.25f), 11U }, std::pair{ glm::vec2(2.5f, 3.5f), 10U } })
			{
				CAPTURE(triangle);
				const std::optional<float> height = SampleTerrainHeight(*heightfield, extent, local);
				const std::optional<glm::vec3> normal = SampleTerrainNormal(*heightfield, extent, local);
				REQUIRE(height.has_value());
				REQUIRE(normal.has_value());
				const glm::vec3 surface = translation + glm::vec3(local.x, *height, local.y);
				const SceneRaycastRequest down{ .Ray = { surface + glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f) }, .MaxDistance = 100.0f };
				const auto result = RaycastScene(fixture.GetScene(), assets, {}, down);
				REQUIRE(result.has_value());
				REQUIRE(result->has_value());
				const SceneRaycastHit& hit = **result;
				CHECK(hit.Entity == terrain.GetUUID());
				CHECK(hit.Submesh == 0);
				CHECK(hit.Triangle == triangle);
				CHECK(glm::length(hit.Position - surface) < 1e-4f);
				CHECK(glm::length(hit.Normal - *normal) < 1e-5f);
				CHECK(hit.Distance == doctest::Approx(30.0).epsilon(1e-6));
				// From below, the two-sided surface turns its normal against the ray.
				const SceneRaycastRequest up{ .Ray = { surface - glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) }, .MaxDistance = 100.0f };
				const auto under = RaycastScene(fixture.GetScene(), assets, {}, up);
				REQUIRE(under.has_value());
				REQUIRE(under->has_value());
				CHECK(glm::length((*under)->Normal + *normal) < 1e-5f);
			}
		}

		TEST_CASE("Raycast: a mesh in front of a terrain is hit first, and a mesh buried below it is not")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryHeightfield, CreateRef<HeightfieldData>(MakeFlatHeightfield(5)));
			assets.Publish(FlatQueryMesh, MakeFlatQueryMesh());
			const Entity terrain = AddQueryTerrain(fixture.GetScene(), UUID(70), glm::vec2(8.0f), glm::vec3(0.0f));
			Entity rock = fixture.GetScene().CreateEntityWithID(UUID(60), "Rock");
			rock.AddComponent<MeshRendererComponent>().Mesh.SetHandle(FlatQueryMesh);
			rock.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Translation = glm::vec3(4.0f, 1.0f, 4.0f);
			});
			const SceneRaycastRequest down{ .Ray = { glm::vec3(4.0f, 10.0f, 4.0f), glm::vec3(0.0f, -1.0f, 0.0f) }, .MaxDistance = 100.0f };
			const auto above = RaycastScene(fixture.GetScene(), assets, {}, down);
			REQUIRE(above.has_value());
			REQUIRE(above->has_value());
			CHECK((*above)->Entity == rock.GetUUID());
			CHECK((*above)->Distance == 9.0f);
			rock.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Translation = glm::vec3(4.0f, -1.0f, 4.0f);
			});
			const auto buried = RaycastScene(fixture.GetScene(), assets, {}, down);
			REQUIRE(buried.has_value());
			REQUIRE(buried->has_value());
			CHECK((*buried)->Entity == terrain.GetUUID());
			CHECK((*buried)->Distance == 10.0f);
			CHECK((*buried)->Position == glm::vec3(4.0f, 0.0f, 4.0f));
			CHECK((*buried)->Normal == glm::vec3(0.0f, 1.0f, 0.0f));
		}

		TEST_CASE("Raycast: a ray past a terrain's heightfield misses it, and a terrain without its heightfield is skipped")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryHeightfield, MakeQueryHeightfield(false));
			const Entity terrain = AddQueryTerrain(fixture.GetScene(), UUID(50), glm::vec2(8.0f), glm::vec3(0.0f));
			const glm::vec3 down(0.0f, -1.0f, 0.0f);
			// Beside each edge of the square, outside [0, 8] x [0, 8], over its box, and pointing away from it.
			for (const glm::vec3 origin : { glm::vec3(-0.01f, 30.0f, 4.0f), glm::vec3(8.01f, 30.0f, 4.0f), glm::vec3(4.0f, 30.0f, -0.01f), glm::vec3(4.0f, 30.0f, 8.01f) })
				CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { origin, down }, .MaxDistance = 100.0f })->has_value());
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(-5.0f, 9.9f, 4.0f), glm::vec3(1.0f, 0.0f, 0.0f) }, .MaxDistance = 100.0f })->has_value());
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(4.0f, 30.0f, 4.0f), -down }, .MaxDistance = 100.0f })->has_value());
			// Within the square, it is hit; clipped by MaxDistance, it is not.
			CHECK(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(7.99f, 30.0f, 0.01f), down }, .MaxDistance = 100.0f })->has_value());
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(4.0f, 30.0f, 4.0f), down }, .MaxDistance = 10.0f })->has_value());
			// A heightfield that does not load (or a Terrain without one) draws nothing, and is never hit.
			terrain.Patch<TerrainComponent>([](TerrainComponent& t)
			{
				t.Heightfield = TypedAssetHandle<AssetType::Heightfield>(AssetHandle(0x5151000000000001ull));
			});
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(4.0f, 30.0f, 4.0f), down }, .MaxDistance = 100.0f })->has_value());
			terrain.Patch<TerrainComponent>([](TerrainComponent& t)
			{
				t.Heightfield = {};
			});
			CHECK_FALSE(RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { glm::vec3(4.0f, 30.0f, 4.0f), down }, .MaxDistance = 100.0f })->has_value());
		}

		TEST_CASE("Raycast: a scaled and rotated terrain is hit where its entity's transform carries its surface")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			const Ref<HeightfieldData> heightfield = MakeQueryHeightfield(true);
			assets.Publish(QueryHeightfield, heightfield);
			Entity terrain = AddQueryTerrain(fixture.GetScene(), UUID(50), glm::vec2(8.0f, 6.0f), glm::vec3(-3.0f, 1.0f, 2.0f));
			terrain.Patch<TransformComponent>([](TransformComponent& t)
			{
				t.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)) * glm::angleAxis(glm::radians(20.0f), glm::vec3(1.0f, 0.0f, 0.0f));
				t.Scale = glm::vec3(2.0f, 0.5f, 1.5f);
			});
			const glm::mat4 world = ComputeRenderedWorldMatrix(terrain, 1.0f);
			const TerrainExtent extent{ .Size = glm::vec2(8.0f, 6.0f), .HeightScale = 10.0f };
			const glm::vec2 local(3.0f, 4.5f);
			const std::optional<float> height = SampleTerrainHeight(*heightfield, extent, local);
			const std::optional<glm::vec3> localNormal = SampleTerrainNormal(*heightfield, extent, local);
			REQUIRE(height.has_value());
			REQUIRE(localNormal.has_value());
			const glm::vec3 target(world * glm::vec4(local.x, *height, local.y, 1.0f));
			const glm::vec3 normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(world))) * *localNormal);
			// An oblique ray from the surface's side (the planar ramp hides no other part of it).
			const glm::vec3 origin = target + 25.0f * glm::normalize(normal + glm::vec3(0.3f, 0.0f, -0.2f));
			const auto result = RaycastScene(fixture.GetScene(), assets, {}, { .Ray = { origin, target - origin }, .MaxDistance = 100.0f });
			REQUIRE(result.has_value());
			REQUIRE(result->has_value());
			CHECK((*result)->Entity == terrain.GetUUID());
			CHECK(glm::length((*result)->Position - target) < 1e-4f);
			CHECK(glm::length((*result)->Normal - normal) < 1e-5f);
			CHECK((*result)->Distance == doctest::Approx(glm::length(target - origin)).epsilon(1e-6));
		}

		TEST_CASE("Raycast: a terrain's own TerrainCollider gives it its layer, below a RigidBody's")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			assets.Publish(QueryHeightfield, CreateRef<HeightfieldData>(MakeFlatHeightfield(5)));
			Scene& scene = fixture.GetScene();
			Entity terrain = AddQueryTerrain(scene, UUID(50), glm::vec2(8.0f), glm::vec3(0.0f));
			const std::vector<std::string> names{ "Default", "Ground", "World" };
			const auto layers = PhysicsLayerTable::Create(names, {});
			REQUIRE(layers);
			SceneRaycastRequest ray{ .Ray = { glm::vec3(4.0f, 30.0f, 4.0f), glm::vec3(0.0f, -1.0f, 0.0f) }, .MaxDistance = 100.0f, .LayerMask = 1 };
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
			terrain.AddComponent<TerrainColliderComponent>().Layer = "Ground";
			CHECK_FALSE(RaycastScene(scene, assets, *layers, ray)->has_value());
			ray.LayerMask = 2;
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
			terrain.AddComponent<RigidBodyComponent>().Layer = "World";
			CHECK_FALSE(RaycastScene(scene, assets, *layers, ray)->has_value());
			ray.LayerMask = 4;
			CHECK(RaycastScene(scene, assets, *layers, ray)->has_value());
		}
	}

}
