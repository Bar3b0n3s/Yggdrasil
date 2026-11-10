#include "TestsPCH.h"

#include "Engine/Scene/EntityBounds.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <vector>

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

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27, stream C): a deformed mesh is bounded as
		// the renderer draws it: a SkinnedMesh's skin by its driving Animator's pose (the skin matrices in the Animator
		// entity's world space, whatever the mesh entity's own transform) with the pose's morph channel for its path
		// overriding its weights, its bind pose without a pose, and a mesh without a SkinnedMesh at its targets' defaults.
		TEST_CASE("EntityBounds: a skinned mesh is bounded by its Animator's pose, and a morphed mesh by its weights")
		{
			Test::InMemoryAssetManager assets;
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A unit quad facing +Z, its left column skinned to joint 0 and its right to joint 1, with a target pushing it 1 along +Z.
			const auto makeQuad = [](bool skinned, float defaultWeight)
			{
				auto mesh = CreateRef<MeshData>();
				for (const glm::vec2 corner : { glm::vec2(0, 0), glm::vec2(1, 0), glm::vec2(1, 1), glm::vec2(0, 1) })
				{
					mesh->Vertices.push_back({ .Position = glm::vec3(corner, 0.0f), .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = corner });
					mesh->Bounds.Extend(glm::vec3(corner, 0.0f));
					if (skinned)
						mesh->Skin.push_back({ .Joints = { static_cast<uint16_t>(corner.x), 0, 0, 0 }, .Weights = { 1, 0, 0, 0 } });
				}
				mesh->SkinJointCount = skinned ? 2 : 0;
				mesh->Indices = { 0, 1, 2, 0, 2, 3 };
				mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
				mesh->Slots = { {} };
				mesh->MorphTargets = { MeshMorphTarget{ .Name = "Push", .DefaultWeight = defaultWeight, .Deltas = std::vector<MeshMorphDelta>(4, MeshMorphDelta{ .Position = { 0, 0, 1 } }) } };
				return mesh;
			};
			const AssetHandle skinnedQuad(0xB0D1000000000001ULL);
			const AssetHandle morphedQuad(0xB0D1000000000002ULL);
			assets.Publish(skinnedQuad, makeQuad(true, 0.0f));
			assets.Publish(morphedQuad, makeQuad(false, 2.0f));

			Entity rig = scene.CreateEntity("Rig");
			rig.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
			});
			rig.AddComponent<AnimatorComponent>();
			Entity body = scene.CreateEntity("Body", rig);
			body.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.0f, 0.0f);
			});
			body.AddComponent<MeshRendererComponent>().Mesh.SetHandle(skinnedQuad);
			body.AddComponent<SkinnedMeshComponent>(SkinnedMeshComponent{ .MorphWeights = { 0.0f } });
			AnimationPoseComponent pose;
			pose.Skin = { glm::mat4(1.0f), glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, 0.0f)) };
			pose.JointModel = pose.Skin;
			pose.Morphs = { AnimationMorphWeights{ .Target = "Body", .Weights = { 1.5f } } };
			rig.AddComponent<AnimationPoseComponent>(pose);

			// Each joint's column, pushed by the target at full weight and half a unit further for the pose's weight 1.5, carried
			// by the rig's world matrix and the joint's skin matrix: joint 1's column is 5 units up.
			const std::optional<Aabb> posed = ComputeEntityWorldBounds(body, assets);
			REQUIRE(posed.has_value());
			CHECK(posed->Min == glm::vec3(10.0f, 0.0f, -1.5f));
			CHECK(posed->Max == glm::vec3(11.0f, 6.0f, 1.5f));

			// Without a pose the skin draws its bind pose at the body's transform, with the component's weight 0.
			rig.RemoveComponent<AnimationPoseComponent>();
			const std::optional<Aabb> bind = ComputeEntityWorldBounds(body, assets);
			REQUIRE(bind.has_value());
			CHECK(bind->Min == glm::vec3(10.0f, 1.0f, 0.0f));
			CHECK(bind->Max == glm::vec3(11.0f, 2.0f, 0.0f));

			// A morphed mesh without a SkinnedMesh draws at its target's default weight, 2: pushed up to 2 units either way.
			Entity morphed = scene.CreateEntity("Morphed");
			morphed.AddComponent<MeshRendererComponent>().Mesh.SetHandle(morphedQuad);
			const std::optional<Aabb> defaulted = ComputeEntityWorldBounds(morphed, assets);
			REQUIRE(defaulted.has_value());
			CHECK(defaulted->Min == glm::vec3(0.0f, 0.0f, -2.0f));
			CHECK(defaulted->Max == glm::vec3(1.0f, 1.0f, 2.0f));
		}
	}

}
