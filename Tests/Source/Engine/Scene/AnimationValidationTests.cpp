#include "TestsPCH.h"

#include "Engine/Scene/AnimationValidation.h"

#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <vector>

// The animation checks of a scene (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 30, stream B). Skipped until
// stream B implements them.

namespace Engine {

	namespace {

		// A MeshRenderer drawing `mesh` with its default materials.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

		// A triangle skinned to `jointCount` joints (every vertex on the last joint).
		Ref<MeshData> MakeSkinnedTriangle(uint32_t jointCount)
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { 0.0f, 0.0f, 0.0f } }, { .Position = { 1.0f, 0.0f, 0.0f } }, { .Position = { 0.0f, 1.0f, 0.0f } } };
			for (const MeshVertex& vertex : mesh->Vertices)
				mesh->Bounds.Extend(vertex.Position);
			mesh->Indices = { 0, 1, 2 };
			mesh->Submeshes.push_back({ .IndexCount = 3, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			mesh->SkinJointCount = jointCount;
			const uint16_t last = static_cast<uint16_t>(jointCount - 1);
			mesh->Skin.assign(3, MeshSkinVertex{ .Joints = { last, 0, 0, 0 } });
			return mesh;
		}

		Ref<SkeletonData> MakeSkeleton(uint32_t jointCount)
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			for (uint32_t index = 0; index < jointCount; ++index)
				skeleton->Joints.push_back({ .Name = "Joint" + std::to_string(index), .Parent = static_cast<int32_t>(index) - 1 });
			return skeleton;
		}

	}

	TEST_SUITE("Scene")
	{
		// A SkinnedMesh whose mesh has a skin needs an Animator on its entity or an ancestor whose skeleton holds the skin's
		// joints; a morph-only mesh needs none, and an unloaded mesh or skeleton is the asset checks' to report.
		TEST_CASE("AnimationValidation: a SkinnedMesh without an Animator and a skeleton with too few joints are reported" * doctest::skip(true))
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle mesh(0x3e5);
			const AssetHandle skeleton(0x5e1);
			assets.Publish(mesh, MakeSkinnedTriangle(3), "Assets/Models/Hero.gltf");
			assets.Publish(skeleton, MakeSkeleton(2), "Assets/Models/Hero.gltf");

			Entity orphan = scene.CreateEntity("Orphan");
			orphan.AddComponent<MeshRendererComponent>(MakeMeshRenderer(mesh));
			orphan.AddComponent<SkinnedMeshComponent>();
			Entity rig = scene.CreateEntity("Rig");
			rig.AddComponent<AnimatorComponent>(AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(skeleton) });
			Entity body = scene.CreateEntity("Body", rig);
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(mesh));
			body.AddComponent<SkinnedMeshComponent>();

			const std::vector<AnimationSceneIssue> issues = FindAnimationSceneIssues(scene, assets);
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Code == AnimationNoAnimatorCode);
			CHECK(issues[0].Entity == orphan.GetUUID());
			CHECK(issues[0].Component == "SkinnedMesh");
			CHECK(issues[0].Severity == DiagnosticSeverity::Warning);
			CHECK(issues[1].Code == AnimationSkeletonMismatchCode);
			CHECK(issues[1].Entity == body.GetUUID());

			// A skeleton that holds the skin's joints is no finding.
			assets.Publish(skeleton, MakeSkeleton(3), "Assets/Models/Hero.gltf");
			const std::vector<AnimationSceneIssue> fixed = FindAnimationSceneIssues(scene, assets);
			REQUIRE(fixed.size() == 1);
			CHECK(fixed[0].Code == AnimationNoAnimatorCode);
		}
	}

}
