#include "TestsPCH.h"

#include "Engine/Scene/AnimationSystem.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SkinnedMeshComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

// The AnimationSystem phase, the editor's posing and the joint query (M23, C23.3, C-SKEL, C-ANIM-BLEND;
// Docs/Decisions/0034-m23-contract.md decision 29, stream B).

namespace Engine {

	namespace {

		constexpr AssetHandle SkeletonHandle(0x5ce1e70000000001);
		constexpr AssetHandle WalkHandle(0xa41c000000000001);
		constexpr AssetHandle WaveHandle(0xa41c000000000002);
		constexpr AssetHandle FlickerHandle(0xa41c000000000003);

		// Hips at y = 1 with a hand one unit above them.
		Ref<SkeletonData> MakeSkeleton()
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			skeleton->Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)) });
			skeleton->Joints.push_back({ .Name = "Hand", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -2.0f, 0.0f)) });
			return skeleton;
		}

		// A one-second walk: the hips move 2 m along +Z (linear), with an event "Step" at 0.5 s.
		Ref<AnimationClipData> MakeWalk()
		{
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 1.0f;
			clip->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 2.0f } });
			clip->Events = { AnimationEvent{ .Name = "Step", .Time = 0.5f } };
			return clip;
		}

		// A half-second wave of the hand: 0.5 m along +X (linear), with an event "Wave" at 0.25 s.
		Ref<AnimationClipData> MakeWave()
		{
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 0.5f;
			clip->Channels.push_back(AnimationChannel{ .Target = "Hand", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 0.5f }, .Values = { 0.0f, 1.0f, 0.0f, 0.5f, 1.0f, 0.0f } });
			clip->Events = { AnimationEvent{ .Name = "Wave", .Time = 0.25f } };
			return clip;
		}

		void PublishRig(Test::InMemoryAssetManager& assets)
		{
			assets.Publish(SkeletonHandle, MakeSkeleton(), "Assets/Models/Hero.gltf");
			assets.Publish(WalkHandle, MakeWalk(), "Assets/Models/Hero.gltf");
			assets.Publish(WaveHandle, MakeWave(), "Assets/Models/Hero.gltf");
		}

		AnimatorComponent MakeAnimator(AnimatorRootMotion rootMotion = AnimatorRootMotion::None)
		{
			AnimatorComponent animator;
			animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle);
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WalkHandle) });
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(WaveHandle), .Weight = 0.5f });
			animator.RootMotion = rootMotion;
			return animator;
		}

		class RecordingListener final : public IAnimationEventListener
		{
		public:
			void OnAnimationEvent(const AnimationEventRecord& event) override { Events.push_back(event); }

			std::vector<AnimationEventRecord> Events;
		};

		// Records each event and destroys `target`'s entity at the first one (a sword hit on an enemy).
		class DestroyingListener final : public IAnimationEventListener
		{
		public:
			DestroyingListener(Scene& scene, UUID target)
				: m_Scene(&scene), m_Target(target)
			{
			}

			void OnAnimationEvent(const AnimationEventRecord& event) override
			{
				Events.push_back(event);
				const Entity target = m_Scene->FindEntityByID(m_Target);
				if (target.IsValid())
					m_Scene->DestroyEntity(target);
			}

			std::vector<AnimationEventRecord> Events;
		private:
			Scene* m_Scene = nullptr; // documented back-reference: the test's scene
			UUID m_Target{};
		};

		constexpr double FixedDelta = 1.0 / 60.0;

		// A triangle skinned to `jointCount` joints (every vertex on the last one), with the morph targets "Smile" (default
		// weight 0.25) and "Blink" (default 0.75).
		Ref<MeshData> MakeFace(uint32_t jointCount)
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { 0.0f, 0.0f, 0.0f } }, { .Position = { 1.0f, 0.0f, 0.0f } }, { .Position = { 0.0f, 1.0f, 0.0f } } };
			for (const MeshVertex& vertex : mesh->Vertices)
				mesh->Bounds.Extend(vertex.Position);
			mesh->Indices = { 0, 1, 2 };
			mesh->Submeshes.push_back({ .IndexCount = 3, .Bounds = mesh->Bounds });
			mesh->Slots.push_back({ .Name = "Default" });
			mesh->SkinJointCount = jointCount;
			mesh->Skin.assign(3, MeshSkinVertex{ .Joints = { static_cast<uint16_t>(jointCount - 1), 0, 0, 0 } });
			mesh->MorphTargets.push_back({ .Name = "Smile", .DefaultWeight = 0.25f, .Deltas = std::vector<MeshMorphDelta>(3) });
			mesh->MorphTargets.push_back({ .Name = "Blink", .DefaultWeight = 0.75f, .Deltas = std::vector<MeshMorphDelta>(3) });
			return mesh;
		}

		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

	}

	TEST_SUITE("Scene")
	{
		// Each playing Animator's layers advance by the fixed delta times their Speed, written back to the component, and the
		// blended pose lands in its AnimationPoseComponent; a paused Animator keeps its times but is still posed at them; an
		// entity without an Animator gains no pose.
		TEST_CASE("AnimationSystem: the phase advances playing animators in canonical order and writes their poses")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(MakeAnimator());
			Entity statue = scene.CreateEntity("Statue");
			AnimatorComponent paused = MakeAnimator();
			paused.Playing = false;
			paused.Layers[0].Time = 0.5f;
			statue.AddComponent<AnimatorComponent>(paused);
			Entity prop = scene.CreateEntity("Prop");
			TransformSystem::Update(scene);

			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			for (uint64_t tick = 0; tick < 30; ++tick)
				(*system)->Step(SimStep::FromTick(tick, FixedDelta));
			// 30 ticks of 1/60 s: the walk at 0.5 s, the wave at its 0.5 s end, wrapped back to 0 or just before it (the sum
			// of thirty float steps).
			const AnimatorComponent& walked = hero.GetComponent<AnimatorComponent>();
			CHECK(std::abs(walked.Layers[0].Time - 0.5f) < 1e-5f);
			CHECK((std::abs(walked.Layers[1].Time) < 1e-5f || std::abs(walked.Layers[1].Time - 0.5f) < 1e-5f));
			REQUIRE(hero.HasComponent<AnimationPoseComponent>());
			const AnimationPoseComponent& pose = hero.GetComponent<AnimationPoseComponent>();
			REQUIRE(pose.JointModel.size() == 2);
			REQUIRE(pose.Skin.size() == 2);
			CHECK(pose.Tick == 29);
			// The hips half way along the walk: (0, 1, 1).
			CHECK(std::abs(pose.JointModel[0][3][2] - 1.0f) < 1e-5f);
			CHECK(statue.GetComponent<AnimatorComponent>().Layers[0].Time == 0.5f);
			REQUIRE(statue.HasComponent<AnimationPoseComponent>());
			CHECK(std::abs(statue.GetComponent<AnimationPoseComponent>().JointModel[0][3][2] - 1.0f) < 1e-5f);
			CHECK_FALSE(prop.HasComponent<AnimationPoseComponent>());
			CHECK((*system)->GetDiagnostics().empty());
		}

		// Apply moves the entity by the tick's planar delta and holds the hips at the clip's start; Extract holds them too but
		// leaves the entity, reporting the delta in the pose; None keeps the motion in the pose.
		TEST_CASE("AnimationSystem: root motion Apply moves the entity, Extract reports it and None keeps it in the pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity applied = scene.CreateEntity("Applied");
			applied.AddComponent<AnimatorComponent>(MakeAnimator(AnimatorRootMotion::Apply));
			Entity extracted = scene.CreateEntity("Extracted");
			extracted.AddComponent<AnimatorComponent>(MakeAnimator(AnimatorRootMotion::Extract));
			Entity kept = scene.CreateEntity("Kept");
			kept.AddComponent<AnimatorComponent>(MakeAnimator(AnimatorRootMotion::None));
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			for (uint64_t tick = 0; tick < 15; ++tick)
				(*system)->Step(SimStep::FromTick(tick, FixedDelta));
			// A quarter second of the walk: 0.5 m along +Z.
			CHECK(std::abs(applied.GetComponent<TransformComponent>().Translation.z - 0.5f) < 1e-4f);
			CHECK(std::abs(applied.GetComponent<AnimationPoseComponent>().JointModel[0][3][2]) < 1e-6f);
			CHECK(extracted.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(std::abs(extracted.GetComponent<AnimationPoseComponent>().JointModel[0][3][2]) < 1e-6f);
			CHECK(std::abs(extracted.GetComponent<AnimationPoseComponent>().RootMotion.Translation.z - 2.0f * static_cast<float>(FixedDelta)) < 1e-5f);
			CHECK(kept.GetComponent<TransformComponent>().Translation == glm::vec3(0.0f));
			CHECK(std::abs(kept.GetComponent<AnimationPoseComponent>().JointModel[0][3][2] - 0.5f) < 1e-4f);
			CHECK(kept.GetComponent<AnimationPoseComponent>().RootMotion == RootMotionDelta{});
		}

		// Events reach the listener after the entity's pose is written, in canonical entity order, then layer order, then
		// firing order within a layer, each with its tick.
		TEST_CASE("AnimationSystem: events reach the listener in canonical, layer and firing order")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity first = scene.CreateEntity("First");
			first.AddComponent<AnimatorComponent>(MakeAnimator());
			Entity second = scene.CreateEntity("Second");
			second.AddComponent<AnimatorComponent>(MakeAnimator());
			TransformSystem::Update(scene);
			RecordingListener listener;
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->SetEventListener(&listener);
			// One step of 0.8 s: the walk passes Step (0.5); the wave passes Wave (0.25), wraps at its 0.5 s end and passes it
			// again at 0.3 s.
			(*system)->Step(SimStep{ .Tick = 0, .FixedDelta = 0.8, .Time = 0.0 });
			REQUIRE(listener.Events.size() == 6);
			CHECK(listener.Events[0] == AnimationEventRecord{ .Entity = first.GetUUID(), .Layer = 0, .Clip = WalkHandle, .Name = "Step", .Time = 0.5f, .Tick = 0 });
			CHECK(listener.Events[1].Name == "Wave");
			CHECK(listener.Events[1].Layer == 1);
			CHECK(listener.Events[2].Name == "Wave");
			CHECK(listener.Events[3].Entity == second.GetUUID());
			CHECK(listener.Events[3].Name == "Step");
			// Without a listener (Simulate) the events are collected and dropped.
			(*system)->SetEventListener(nullptr);
			(*system)->Step(SimStep{ .Tick = 1, .FixedDelta = 0.8, .Time = 0.8 });
			CHECK(listener.Events.size() == 6);
		}

		// The phase steps every Animator before it dispatches: a callback that destroys an Animator's entity later in canonical
		// order finds that Animator already advanced and posed this tick, its events of the tick are dropped, and until the
		// flush it is skipped (pending destruction), so what a callback does takes effect from the next tick whatever the
		// entities' order.
		TEST_CASE("AnimationSystem: events are dispatched after every Animator steps, and an event that destroys another Animator drops its events")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity first = scene.CreateEntity("First");
			first.AddComponent<AnimatorComponent>(MakeAnimator());
			Entity second = scene.CreateEntity("Second");
			second.AddComponent<AnimatorComponent>(MakeAnimator());
			TransformSystem::Update(scene);

			DestroyingListener listener(scene, second.GetUUID());
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->SetEventListener(&listener);
			(*system)->Step(SimStep{ .Tick = 0, .FixedDelta = 0.8, .Time = 0.0 });
			// The first Animator's three events arrive; its first destroys the second, whose own three are dropped.
			REQUIRE(listener.Events.size() == 3);
			for (const AnimationEventRecord& event : listener.Events)
				CHECK(event.Entity == first.GetUUID());
			REQUIRE(scene.GetPendingDestroyCount() > 0);
			// The second was stepped before the dispatch: its walk is at 0.8 s and its pose written.
			CHECK(std::abs(second.GetComponent<AnimatorComponent>().Layers[0].Time - 0.8f) < 1e-6f);
			CHECK(second.HasComponent<AnimationPoseComponent>());
			// Pending destruction, it is skipped at the next step: its time stays while the first's advances.
			(*system)->Step(SimStep{ .Tick = 1, .FixedDelta = 0.1, .Time = 0.8 });
			CHECK(std::abs(second.GetComponent<AnimatorComponent>().Layers[0].Time - 0.8f) < 1e-6f);
			CHECK(std::abs(first.GetComponent<AnimatorComponent>().Layers[0].Time - 0.9f) < 1e-6f);
		}

		// A fuzz-shaped clip (the shortest a clip may be, MaxAnimationEvents events at its start and end, played looping at the
		// fastest speed) passes about 1,700 ends in one step: the layer counts at most MaxAnimationWrapsPerStep of them, and the
		// Animator delivers at most MaxAnimationEventsPerStep events, the rest dropped with one warning for the Animator, never
		// again.
		TEST_CASE("AnimationSystem: an Animator delivers at most MaxAnimationEventsPerStep events a step and warns once when it drops some")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Ref<AnimationClipData> flicker = CreateRef<AnimationClipData>();
			flicker->Duration = MinAnimationClipDuration;
			flicker->Channels.push_back(AnimationChannel{ .Target = "Hand", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { 0.0f, 1.0f, 0.0f } });
			for (uint32_t index = 0; index < MaxAnimationEvents; ++index)
				flicker->Events.push_back(AnimationEvent{ .Name = "Flick", .Time = index < MaxAnimationEvents / 2 ? 0.0f : MinAnimationClipDuration });
			assets.Publish(FlickerHandle, flicker, "Assets/Models/Hero.gltf");
			Entity hero = scene.CreateEntity("Hero");
			AnimatorComponent animator;
			animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle);
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(FlickerHandle), .Speed = MaxAnimatorSpeed });
			hero.AddComponent<AnimatorComponent>(animator);
			TransformSystem::Update(scene);

			RecordingListener listener;
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->SetEventListener(&listener);
			const Test::ExpectLog dropped(LogLevel::Warn, hero.GetUUID().ToString());
			(*system)->Step(SimStep::FromTick(0, FixedDelta));
			CHECK(listener.Events.size() == MaxAnimationEventsPerStep);
			CHECK(dropped.GetMatchCount() == 1);
			(*system)->Step(SimStep::FromTick(1, FixedDelta));
			CHECK(listener.Events.size() == 2 * MaxAnimationEventsPerStep);
			CHECK(dropped.GetMatchCount() == 1);
			// The layer's time stays exact within the clip.
			const float time = hero.GetComponent<AnimatorComponent>().Layers[0].Time;
			CHECK(time >= 0.0f);
			CHECK(time <= MinAnimationClipDuration);
		}

		// The editor's preview poses an Animator at its layers' times plus a preview time without touching the component, and
		// the joint query reads the last pose in world space.
		TEST_CASE("AnimationSystem: PoseAnimator samples without advancing, and the joint query reads the pose in world space")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity hero = scene.CreateEntity("Hero");
			hero.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
			});
			hero.AddComponent<AnimatorComponent>(MakeAnimator());
			Entity child = scene.CreateEntity("Body", hero);
			TransformSystem::Update(scene);
			const uint64_t revision = scene.GetRevision();

			REQUIRE(PoseAnimator(hero, assets, 0.5f).has_value());
			CHECK(hero.GetComponent<AnimatorComponent>().Layers[0].Time == 0.0f);
			CHECK(scene.GetRevision() == revision);
			const std::optional<glm::mat4> hand = FindAnimatorJointWorldTransform(hero, "Hand");
			REQUIRE(hand.has_value());
			// The hips at (0, 1, 1) after half the walk, the hand one unit above, the rig at x = 10; the wave at 0.5 s (its end,
			// wrapped to 0) adds nothing.
			CHECK(std::abs((*hand)[3][0] - 10.0f) < 1e-5f);
			CHECK(std::abs((*hand)[3][1] - 2.0f) < 1e-5f);
			CHECK(std::abs((*hand)[3][2] - 1.0f) < 1e-5f);
			CHECK_FALSE(FindAnimatorJointWorldTransform(hero, "Tail").has_value());
			CHECK(FindDrivingAnimator(child) == ConstEntity(hero));
			CHECK_FALSE(FindDrivingAnimator(scene.CreateEntity("Loose")).IsValid());

			AnimatorComponent unloaded = MakeAnimator();
			unloaded.Skeleton = TypedAssetHandle<AssetType::Skeleton>(AssetHandle(0x404));
			hero.Patch<AnimatorComponent>([&unloaded](AnimatorComponent& animator)
			{
				animator = unloaded;
			});
			const Status missing = PoseAnimator(hero, assets);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(hero.HasComponent<AnimationPoseComponent>());
		}

		// A morph layer blends over the SkinnedMesh's own weights, each missing one its target's default (decision 26's base),
		// as a joint blends over its rest pose: at weight 0.5 the face is half way from {0.5, 0.75} to the clip's {1, 0}.
		TEST_CASE("AnimationSystem: a morph layer blends from the SkinnedMesh's weights, the targets' defaults filling the missing ones")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			const AssetHandle faceHandle(0x3e50000000000021);
			assets.Publish(faceHandle, MakeFace(2), "Assets/Models/Hero.gltf");
			const AssetHandle smileHandle(0xa41c000000000004);
			Ref<AnimationClipData> smile = CreateRef<AnimationClipData>();
			smile->Duration = 1.0f;
			smile->Channels.push_back(AnimationChannel{ .Target = "Face", .Path = AnimationPath::Weights, .Interpolation = AnimationInterpolation::Step, .WeightCount = 2, .Times = { 0.0f }, .Values = { 1.0f, 0.0f } });
			assets.Publish(smileHandle, smile, "Assets/Models/Hero.gltf");
			Entity hero = scene.CreateEntity("Hero");
			AnimatorComponent animator;
			animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(SkeletonHandle);
			animator.Layers.push_back(AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(smileHandle), .Weight = 0.5f });
			hero.AddComponent<AnimatorComponent>(animator);
			Entity face = scene.CreateEntity("Face", hero);
			face.AddComponent<MeshRendererComponent>(MakeMeshRenderer(faceHandle));
			face.AddComponent<SkinnedMeshComponent>(SkinnedMeshComponent{ .MorphWeights = { 0.5f } });
			TransformSystem::Update(scene);

			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->Step(SimStep::FromTick(0, FixedDelta));
			const AnimationPoseComponent& pose = hero.GetComponent<AnimationPoseComponent>();
			REQUIRE(pose.Morphs.size() == 1);
			CHECK(pose.Morphs[0].Target == "Face");
			CHECK(pose.Morphs[0].Weights == std::vector<float>{ 0.75f, 0.375f });
			CHECK((*system)->GetDiagnostics().empty());
		}

		// The phase raises the animation checks at run time (decision 30), once per entity and code however many ticks run:
		// a skinned mesh without an Animator above it, and one whose Animator's skeleton holds fewer joints than its skin.
		TEST_CASE("AnimationSystem: a SkinnedMesh without an Animator or with too few joints is reported once at run time")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			const AssetHandle faceHandle(0x3e50000000000022);
			assets.Publish(faceHandle, MakeFace(3), "Assets/Models/Hero.gltf");
			Entity orphan = scene.CreateEntity("Orphan");
			orphan.AddComponent<MeshRendererComponent>(MakeMeshRenderer(faceHandle));
			orphan.AddComponent<SkinnedMeshComponent>();
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(MakeAnimator());
			Entity body = scene.CreateEntity("Body", hero);
			body.AddComponent<MeshRendererComponent>(MakeMeshRenderer(faceHandle));
			body.AddComponent<SkinnedMeshComponent>();
			TransformSystem::Update(scene);

			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const Test::ExpectLog noAnimator(LogLevel::Warn, AnimationNoAnimatorCode);
			const Test::ExpectLog mismatch(LogLevel::Warn, AnimationSkeletonMismatchCode);
			for (uint64_t tick = 0; tick < 3; ++tick)
				(*system)->Step(SimStep::FromTick(tick, FixedDelta));
			const std::span<const AnimationSceneIssue> issues = (*system)->GetDiagnostics();
			REQUIRE(issues.size() == 2);
			CHECK(issues[0].Code == AnimationNoAnimatorCode);
			CHECK(issues[0].Entity == orphan.GetUUID());
			CHECK(issues[1].Code == AnimationSkeletonMismatchCode);
			CHECK(issues[1].Entity == body.GetUUID());
			CHECK(noAnimator.GetMatchCount() == 1);
			CHECK(mismatch.GetMatchCount() == 1);
			// The Animator is still posed: the renderer draws the mismatched mesh in its bind pose.
			CHECK(hero.HasComponent<AnimationPoseComponent>());
		}

		// A session without an asset manager still steps the phase: no skeleton or clip loads, so nothing advances or is posed.
		TEST_CASE("AnimationSystem: without an asset manager no layer advances and nothing is posed")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(MakeAnimator());
			TransformSystem::Update(scene);
			CHECK_FALSE(AnimationSystem::Create({ .RuntimeScene = nullptr }).has_value());
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const uint64_t revision = scene.GetRevision();
			(*system)->Step(SimStep::FromTick(0, FixedDelta));
			CHECK(hero.GetComponent<AnimatorComponent>().Layers[0].Time == 0.0f);
			CHECK_FALSE(hero.HasComponent<AnimationPoseComponent>());
			CHECK(scene.GetRevision() == revision);
		}

		// The editor's posing refuses a preview time that is not a duration and names an Animator without a Skeleton.
		TEST_CASE("AnimationSystem: PoseAnimator refuses a negative or non-finite preview time and an Animator without a Skeleton")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<AnimatorComponent>(MakeAnimator());
			TransformSystem::Update(scene);
			for (const float time : { -0.5f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				const Status refused = PoseAnimator(hero, assets, time);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			CHECK_FALSE(hero.HasComponent<AnimationPoseComponent>());
			REQUIRE(PoseAnimator(hero, assets).has_value());
			REQUIRE(hero.HasComponent<AnimationPoseComponent>());
			hero.Patch<AnimatorComponent>([](AnimatorComponent& animator)
			{
				animator.Skeleton = {};
			});
			const Status missing = PoseAnimator(hero, assets);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(hero.HasComponent<AnimationPoseComponent>());
		}

		// A script can write a new problem every tick, so the system keeps at most MaxAnimationSystemDiagnostics findings: past
		// them a finding is neither kept nor logged, and one warning says so.
		TEST_CASE("AnimationSystem: findings past MaxAnimationSystemDiagnostics are neither kept nor logged, and one warning says so")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle faceHandle(0x3e50000000000023);
			assets.Publish(faceHandle, MakeFace(1), "Assets/Models/Hero.gltf");
			for (size_t index = 0; index < MaxAnimationSystemDiagnostics + 1; ++index)
			{
				Entity orphan = scene.CreateEntity("Orphan");
				orphan.AddComponent<MeshRendererComponent>(MakeMeshRenderer(faceHandle));
				orphan.AddComponent<SkinnedMeshComponent>();
			}
			TransformSystem::Update(scene);
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const Test::ExpectLog findings(LogLevel::Warn, AnimationNoAnimatorCode);
			const Test::ExpectLog reached(LogLevel::Warn, "animation findings reached");
			(*system)->Step(SimStep::FromTick(0, FixedDelta));
			(*system)->Step(SimStep::FromTick(1, FixedDelta));
			CHECK((*system)->GetDiagnostics().size() == MaxAnimationSystemDiagnostics);
			CHECK(findings.GetMatchCount() == MaxAnimationSystemDiagnostics);
			CHECK(reached.GetMatchCount() == 1);
		}
	}

}
