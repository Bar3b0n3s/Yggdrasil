#include "TestsPCH.h"

#include "EditorCore/Animation/AnimationPreview.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/EditorTestFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <cmath>
#include <limits>

// The editor's animation preview (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 32, stream D): an open edit
// scene's Animator posed at a time without changing the scene, the preview belonging to that scene among M20's open scenes.

namespace Engine {

	namespace {

		const AssetHandle SkeletonHandle(0x5ce1e70000000021);
		const AssetHandle WalkHandle(0xa41c000000000021);

		void PublishRig(Test::InMemoryAssetManager& assets)
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			skeleton->Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, 1.0f, 0.0f } });
			assets.Publish(SkeletonHandle, skeleton, "Assets/Models/Hero.gltf");
			Ref<AnimationClipData> walk = CreateRef<AnimationClipData>();
			walk->Duration = 2.0f;
			walk->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 2.0f }, .Values = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 4.0f } });
			assets.Publish(WalkHandle, walk, "Assets/Models/Hero.gltf");
		}

		AnimatorComponent MakeWalker()
		{
			return AnimatorComponent{ .Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle),
				.Layers = { AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) } } };
		}

		// An open project with its Main scene open and active.
		void OpenMainScene(Test::EditorTestFixture& fixture)
		{
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
		}

	}

	TEST_SUITE("EditorCore")
	{
		// Begin poses the Animator at once without touching the scene's revision or the component; playing advances the time by
		// the frame's seconds; End removes the pose, and a preview whose entity is gone ends by itself.
		TEST_CASE("AnimationPreview: previewing an Animator poses the edit scene without changing it, and ending restores the bind pose")
		{
			Test::EditorTestFixture fixture("AnimationPreview");
			OpenMainScene(fixture);
			EditorContext& editor = fixture.GetEditor();
			const OpenSceneId main = editor.GetActiveSceneId();
			Scene& scene = editor.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(MakeWalker());
			Entity statue = scene.CreateEntity("Statue");
			TransformSystem::Update(scene);
			const uint64_t revision = scene.GetRevision();

			AnimationPreview preview;
			const Result<AnimationPreviewState> started = preview.Begin(editor, main, hero.GetUUID(), assets, 0.5f, false);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			CHECK(started->Scene == main);
			CHECK(started->ActiveScene == main);
			CHECK(started->Entity == hero.GetUUID());
			CHECK(started->Duration == 2.0f);
			REQUIRE(hero.HasComponent<AnimationPoseComponent>());
			CHECK(std::abs(hero.GetComponent<AnimationPoseComponent>().JointModel[0][3][2] - 1.0f) < 1e-5f);
			CHECK(scene.GetRevision() == revision);
			CHECK(hero.GetComponent<AnimatorComponent>().Layers[0].Time == 0.0f);

			// Paused, a frame changes nothing; playing, it advances the time by the frame's seconds.
			preview.Update(editor, assets, 0.25);
			CHECK(preview.GetState()->Time == 0.5f);
			REQUIRE(preview.Begin(editor, main, hero.GetUUID(), assets, 0.5f, true).has_value());
			preview.Update(editor, assets, 0.25);
			CHECK(preview.GetState()->Time == 0.75f);
			CHECK(std::abs(hero.GetComponent<AnimationPoseComponent>().JointModel[0][3][2] - 1.5f) < 1e-5f);

			preview.End(editor);
			CHECK_FALSE(preview.GetState().has_value());
			CHECK_FALSE(hero.HasComponent<AnimationPoseComponent>());
			CHECK(scene.GetRevision() == revision);

			// An entity without an Animator, an unknown entity or scene and a time below 0 are refused, and nothing starts.
			const Result<AnimationPreviewState> noAnimator = preview.Begin(editor, main, statue.GetUUID(), assets, 0.0f, false);
			REQUIRE_FALSE(noAnimator.has_value());
			CHECK(noAnimator.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<AnimationPreviewState> noScene = preview.Begin(editor, main + 100, hero.GetUUID(), assets, 0.0f, false);
			REQUIRE_FALSE(noScene.has_value());
			CHECK(noScene.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(preview.Begin(editor, main, hero.GetUUID(), assets, -1.0f, false).has_value());
			CHECK_FALSE(preview.Begin(editor, main, hero.GetUUID(), assets, std::numeric_limits<float>::infinity(), false).has_value());
			CHECK_FALSE(preview.GetState().has_value());

			// A preview whose entity is destroyed ends at the next frame.
			REQUIRE(preview.Begin(editor, main, hero.GetUUID(), assets, 0.0f, false).has_value());
			scene.DestroyEntity(hero);
			preview.Update(editor, assets, 0.0);
			CHECK_FALSE(preview.GetState().has_value());
		}

		// The preview records its scene, so with several open edit scenes a pose is only ever written and removed where it
		// belongs: previewing an Animator of another scene (even an entity of the same id, as a scene saved as a copy holds)
		// removes the first scene's pose; switching the active tab ends the preview at the next frame, removing its pose; and
		// closing its scene ends it with the scene.
		TEST_CASE("AnimationPreview: a preview belongs to its scene, and previewing another scene, switching tabs or closing the scene removes its pose")
		{
			Test::EditorTestFixture fixture("AnimationPreviewScenes");
			OpenMainScene(fixture);
			EditorContext& editor = fixture.GetEditor();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			const OpenSceneId main = editor.GetActiveSceneId();
			Scene& mainScene = editor.GetScene();
			Entity mainHero = mainScene.CreateEntity("Hero");
			mainHero.AddComponent<AnimatorComponent>(MakeWalker());
			TransformSystem::Update(mainScene);
			const Result<VfsPath> copyPath = VfsPath::Parse("project://Assets/Scenes/Copy.scene");
			REQUIRE(copyPath.has_value());
			const Result<OpenSceneId> copy = editor.AddScene(editor.CreateScene("Copy"), *copyPath);
			REQUIRE_MESSAGE(copy.has_value(), copy.error().ToString());
			Scene* copyScene = editor.FindScene(*copy);
			REQUIRE(copyScene != nullptr);
			Entity copyHero = copyScene->CreateEntityWithID(mainHero.GetUUID(), "Hero");
			copyHero.AddComponent<AnimatorComponent>(MakeWalker());
			TransformSystem::Update(*copyScene);
			REQUIRE(editor.ActivateScene(main).has_value());

			// Previewing the copy's hero ends the main scene's preview and removes its pose there, though the ids are equal.
			AnimationPreview preview;
			REQUIRE(preview.Begin(editor, main, mainHero.GetUUID(), assets, 0.5f, false).has_value());
			REQUIRE(mainHero.HasComponent<AnimationPoseComponent>());
			const Result<AnimationPreviewState> moved = preview.Begin(editor, *copy, copyHero.GetUUID(), assets, 0.5f, false);
			REQUIRE_MESSAGE(moved.has_value(), moved.error().ToString());
			CHECK(moved->Scene == *copy);
			CHECK_FALSE(mainHero.HasComponent<AnimationPoseComponent>());
			CHECK(copyHero.HasComponent<AnimationPoseComponent>());

			// Switching the active tab ends the preview at the next frame, removing the pose from the scene that holds it.
			REQUIRE(editor.ActivateScene(*copy).has_value());
			preview.Update(editor, assets, 0.0);
			CHECK_FALSE(preview.GetState().has_value());
			CHECK_FALSE(copyHero.HasComponent<AnimationPoseComponent>());
			CHECK_FALSE(mainHero.HasComponent<AnimationPoseComponent>());

			// A preview of a background scene (a scene target that is not the active tab) runs until the tab switches.
			REQUIRE(preview.Begin(editor, main, mainHero.GetUUID(), assets, 0.25f, false).has_value());
			preview.Update(editor, assets, 0.0);
			CHECK(preview.GetState().has_value());
			CHECK(mainHero.HasComponent<AnimationPoseComponent>());

			// Closing the previewed scene ends the preview with it.
			REQUIRE(editor.CloseScene(main).has_value());
			preview.Update(editor, assets, 0.0);
			CHECK_FALSE(preview.GetState().has_value());
			CHECK_FALSE(copyHero.HasComponent<AnimationPoseComponent>());
			preview.End(editor);
		}
	}

}
