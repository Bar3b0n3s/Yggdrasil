#include "TestsPCH.h"

#include "Engine/Scene/EntityBounds.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>

namespace Engine {

	namespace {

		Entity AddCube(Scene& scene, std::string_view name, const glm::vec3& translation, const glm::vec3& scale)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&translation, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Scale = scale;
			});
			entity.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			return entity;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("EntityBounds: a translated and scaled unit cube has the transformed world AABB")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			const Entity box = AddCube(scene.GetScene(), "Box", glm::vec3(10.0f, 1.0f, 0.0f), glm::vec3(2.0f, 1.0f, 4.0f));
			const std::optional<Aabb> bounds = ComputeEntityWorldBounds(box, assets.GetManager());
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min == glm::vec3(9.0f, 0.5f, -2.0f));
			CHECK(bounds->Max == glm::vec3(11.0f, 1.5f, 2.0f));

			const Entity empty = scene.GetScene().CreateEntity("Empty");
			CHECK_FALSE(ComputeEntityWorldBounds(empty, assets.GetManager()).has_value());
		}

		TEST_CASE("EntityBounds: descendants are included and disabled ones skipped")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			Entity root = scene.GetScene().CreateEntity("Track");
			Entity first = AddCube(scene.GetScene(), "Piece", glm::vec3(0.0f), glm::vec3(1.0f));
			Entity second = AddCube(scene.GetScene(), "Piece", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			Entity hidden = AddCube(scene.GetScene(), "Hidden", glm::vec3(-50.0f, 0.0f, 0.0f), glm::vec3(1.0f));
			REQUIRE(scene.GetScene().SetParent(first, root).has_value());
			REQUIRE(scene.GetScene().SetParent(second, root).has_value());
			REQUIRE(scene.GetScene().SetParent(hidden, root).has_value());
			hidden.SetActive(false);

			const std::optional<Aabb> all = ComputeEntityWorldBounds(root, assets.GetManager());
			REQUIRE(all.has_value());
			CHECK(all->Min == glm::vec3(-0.5f, -0.5f, -0.5f));
			CHECK(all->Max == glm::vec3(5.5f, 0.5f, 0.5f));
			// Without descendants the root has no mesh of its own.
			CHECK_FALSE(ComputeEntityWorldBounds(root, assets.GetManager(), { .IncludeDescendants = false }).has_value());
		}

		TEST_CASE("EntityBounds: a missing mesh counts with the placeholder cube and records the diagnostic")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			const AssetHandle missing(0x123456789ull);
			Entity box = scene.GetScene().CreateEntity("Box");
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 2.0f, 0.0f);
			});
			box.AddComponent<MeshRendererComponent>().Mesh.SetHandle(missing);
			std::optional<Aabb> bounds;
			{
				const Test::ExpectLog logged(LogLevel::Error, missing.ToString());
				bounds = ComputeEntityWorldBounds(box, assets.GetManager());
			}
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min == glm::vec3(-0.5f, 1.5f, -0.5f));
			CHECK(bounds->Max == glm::vec3(0.5f, 2.5f, 0.5f));
			CHECK(std::ranges::any_of(assets.GetManager().GetDiagnostics(), [missing](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == missing && diagnostic.Code == AssetMissingCode;
			}));
			// MeshRenderer.Visible does not matter: an invisible mesh still occupies its space.
			box.Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Visible = false;
				renderer.Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);
			});
			CHECK(ComputeEntityWorldBounds(box, assets.GetManager()).has_value());
		}
	}

}
