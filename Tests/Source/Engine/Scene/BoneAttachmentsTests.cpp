#include "TestsPCH.h"

#include "Engine/Scene/BoneAttachments.h"

#include "Engine/Asset/AnimationClipData.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Components/AnimationPoseComponent.h"
#include "Engine/Scene/Components/AnimatorComponent.h"
#include "Engine/Scene/Components/BoneAttachmentComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/GlmApprox.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Bone attachments (M41, Docs/Decisions/0058-m41-bone-attachments.md decision 3): the attaching pass of the AnimationSystem
// phase, its reach, its determinism and ANIMATION_ATTACHMENT_INVALID. Decision R's rendering is RenderExtractionTests.cpp's.

namespace Engine {

	namespace {

		constexpr AssetHandle SkeletonHandle(0xb0e5000000000001);
		constexpr AssetHandle WalkHandle(0xb0e5000000000002);
		constexpr AssetHandle SwingHandle(0xb0e5000000000003);
		constexpr AssetHandle GrowHandle(0xb0e5000000000004);
		constexpr AssetHandle VanishHandle(0xb0e5000000000005);
		constexpr AssetHandle TallSkeletonHandle(0xb0e5000000000006);
		constexpr AssetHandle OverflowHandle(0xb0e5000000000007);
		constexpr double FixedDelta = 1.0 / 60.0;

		// Hips `hipsHeight` above the Animator's entity with a hand one unit above them.
		Ref<SkeletonData> MakeSkeleton(float hipsHeight)
		{
			Ref<SkeletonData> skeleton = CreateRef<SkeletonData>();
			skeleton->Joints.push_back({ .Name = "Hips", .Translation = { 0.0f, hipsHeight, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -hipsHeight, 0.0f)) });
			skeleton->Joints.push_back({ .Name = "Hand", .Parent = 0, .Translation = { 0.0f, 1.0f, 0.0f }, .InverseBind = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -hipsHeight - 1.0f, 0.0f)) });
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

		// A one-second swing of the hand about +Z, to 90 degrees at 0.5 s and back (linear, so slerped through DetMath), with an
		// event "Strike" at 0.25 s.
		Ref<AnimationClipData> MakeSwing()
		{
			const float half = 0.70710678f;
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 1.0f;
			clip->Channels.push_back(AnimationChannel{ .Target = "Hand", .Path = AnimationPath::Rotation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 0.5f, 1.0f }, .Values = { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, half, half, 0.0f, 0.0f, 0.0f, 1.0f } });
			clip->Events = { AnimationEvent{ .Name = "Strike", .Time = 0.25f } };
			return clip;
		}

		// The hand held at a uniform scale of `scale` (a step key).
		Ref<AnimationClipData> MakeHandScale(float scale)
		{
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 1.0f;
			clip->Channels.push_back(AnimationChannel{ .Target = "Hand", .Path = AnimationPath::Scale, .Interpolation = AnimationInterpolation::Step, .Times = { 0.0f }, .Values = { scale, scale, scale } });
			return clip;
		}

		// M23's milestone review's overflow: the hips' z keyed at 2e38 and -2e38, so every sample between is not finite.
		Ref<AnimationClipData> MakeOverflow()
		{
			Ref<AnimationClipData> clip = CreateRef<AnimationClipData>();
			clip->Duration = 1.0f;
			clip->Channels.push_back(AnimationChannel{ .Target = "Hips", .Path = AnimationPath::Translation, .Interpolation = AnimationInterpolation::Linear, .Times = { 0.0f, 1.0f }, .Values = { 0.0f, 1.0f, 2.0e38f, 0.0f, 1.0f, -2.0e38f } });
			return clip;
		}

		void PublishRig(Test::InMemoryAssetManager& assets)
		{
			assets.Publish(SkeletonHandle, MakeSkeleton(1.0f), "Assets/Models/Warden.gltf");
			assets.Publish(TallSkeletonHandle, MakeSkeleton(2.0f), "Assets/Models/Giant.gltf");
			assets.Publish(WalkHandle, MakeWalk(), "Assets/Models/Warden.gltf");
			assets.Publish(SwingHandle, MakeSwing(), "Assets/Models/Warden.gltf");
			assets.Publish(GrowHandle, MakeHandScale(3.0f), "Assets/Models/Warden.gltf");
			assets.Publish(VanishHandle, MakeHandScale(0.0f), "Assets/Models/Warden.gltf");
			assets.Publish(OverflowHandle, MakeOverflow(), "Assets/Models/Warden.gltf");
		}

		AnimatorLayer MakeLayer(AssetHandle clip, float time = 0.0f)
		{
			return AnimatorLayer{ .Clip = TypedAssetHandle<AssetType::AnimationClip>(clip), .Time = time };
		}

		// An Animator of `skeleton` playing `layers`, each at weight 1.
		AnimatorComponent MakeAnimator(std::vector<AnimatorLayer> layers, AnimatorRootMotion rootMotion = AnimatorRootMotion::None, AssetHandle skeleton = SkeletonHandle)
		{
			AnimatorComponent animator;
			animator.Skeleton = TypedAssetHandle<AssetType::Skeleton>(skeleton);
			animator.Layers = std::move(layers);
			animator.RootMotion = rootMotion;
			return animator;
		}

		glm::quat AboutX(float degrees)
		{
			return TransformSystem::QuaternionFromEulerDegrees(glm::vec3(degrees, 0.0f, 0.0f));
		}

		glm::quat AboutY(float degrees)
		{
			return TransformSystem::QuaternionFromEulerDegrees(glm::vec3(0.0f, degrees, 0.0f));
		}

		void SetPose(Entity entity, const glm::vec3& translation, const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f), const glm::vec3& scale = glm::vec3(1.0f))
		{
			entity.Patch<TransformComponent>([&translation, &rotation, &scale](TransformComponent& transform)
			{
				transform.Translation = translation;
				transform.Rotation = rotation;
				transform.Scale = scale;
			});
		}

		// A child `name` of `parent` attached to `bone` with `offset` and `rotation`.
		Entity AddAttachment(Scene& scene, std::string_view name, Entity parent, std::string_view bone, const glm::vec3& offset = glm::vec3(0.0f),
			const glm::quat& rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f))
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<BoneAttachmentComponent>(BoneAttachmentComponent{ .Bone = std::string(bone), .Offset = offset, .Rotation = rotation });
			return entity;
		}

		Scope<AnimationSystem> CreateSystem(Scene& scene, AssetManager& assets)
		{
			Result<Scope<AnimationSystem>> system = AnimationSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			return std::move(*system);
		}

		// One AnimationSystem phase at `tick`, then the transform update that follows it (the session's
		// PreStepTransformUpdate), so the joint query reads the tick's Animator pose.
		void StepAndUpdate(AnimationSystem& system, Scene& scene, uint64_t tick)
		{
			system.Step(SimStep::FromTick(tick, FixedDelta));
			TransformSystem::Update(scene);
		}

		// `attached` sits at `bone` of `animator`'s last pose with `offset` and `rotation` (the joint's scale is 1 here):
		// its world position within `tolerance` of FindAnimatorJointWorldTransform times the offset, its world rotation the
		// joint's times `rotation`.
		void CheckOnBone(ConstEntity attached, ConstEntity animator, std::string_view bone, const glm::vec3& offset, const glm::quat& rotation,
			float tolerance = 1e-5f)
		{
			const std::optional<glm::mat4> joint = FindAnimatorJointWorldTransform(animator, bone);
			REQUIRE(joint.has_value());
			const glm::vec3 expected(*joint * glm::vec4(offset, 1.0f));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(attached), expected, tolerance));
			const Result<TransformDecomposition> decomposed = TransformSystem::DecomposeMatrix(*joint);
			REQUIRE(decomposed.has_value());
			const glm::quat expectedRotation = decomposed->Rotation * rotation;
			const glm::quat actual = TransformSystem::GetWorldRotation(attached);
			for (const glm::vec3 axis : { glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) })
				CHECK(Test::ApproxEqual(expectedRotation * axis, actual * axis, tolerance));
		}

		bool SameBits(const glm::vec3& a, const glm::vec3& b)
		{
			return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) && std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y)
				&& std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
		}

		// The number of ANIMATION_ATTACHMENT_INVALID findings `issues` holds on `entity`.
		size_t CountFindings(std::span<const AnimationSceneIssue> issues, UUID entity)
		{
			size_t count = 0;
			for (const AnimationSceneIssue& issue : issues)
			{
				if (issue.Code == AnimationAttachmentInvalidCode && issue.Entity == entity)
					++count;
			}
			return count;
		}

		const AnimationSceneIssue* FindIssue(std::span<const AnimationSceneIssue> issues, UUID entity)
		{
			for (const AnimationSceneIssue& issue : issues)
			{
				if (issue.Entity == entity)
					return &issue;
			}
			return nullptr;
		}

		// Moves the character at "/Rig" by MoveCharacter in every FixedUpdate phase after the first (its body exists from
		// the first PreStep).
		class CharacterDriver final : public IPlaySessionObserver
		{
		public:
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase != PlaySessionPhase::FixedUpdate || tick == 0)
					return;
				const UUID character = Test::GetEntityId(session.GetScene(), "/Rig");
				REQUIRE(session.GetPhysics().MoveCharacter(character, glm::vec3(1.5f, 0.0f, 0.75f)).has_value());
			}
		};

		// Records, at each animation event, the event and where the attached entity `Path` stood then.
		class PlacementListener final : public IAnimationEventListener
		{
		public:
			PlacementListener(Scene& scene, std::string path)
				: m_Scene(&scene), m_Path(std::move(path))
			{
			}

			void OnAnimationEvent(const AnimationEventRecord& event) override
			{
				Events.push_back(event);
				Translations.push_back(m_Scene->FindEntityByPath(m_Path).GetComponent<TransformComponent>().Translation);
			}

			std::vector<AnimationEventRecord> Events;
			std::vector<glm::vec3> Translations;
		private:
			Scene* m_Scene = nullptr; // documented back-reference: the test's scene
			std::string m_Path;
		};

	}

	TEST_SUITE("Scene")
	{
		// The Acceptance's first case: across a clip that walks with root motion Apply and swings the hand, the attached entity
		// stays within 1e-5 m of FindAnimatorJointWorldTransform times its Offset, turned by the joint's rotation times its
		// Rotation, between ticks (after the tick's transform update) where both read the tick's pose; so does one on an
		// Animator with no SkinnedMesh (an invisible rig) whose entity is turned and placed in the world. Then in a play
		// session, under a CharacterController parent that MoveCharacter moves later in each tick (in PhysicsPreStep, after the
		// phase): the attachment, placed in the Animator entity's space, is still exact once the step wrote the character
		// back.
		TEST_CASE("BoneAttachments: an attached entity follows its bone across a clip")
		{
			const glm::vec3 offset(0.0f, 0.5f, 0.1f);
			const glm::quat rotation = AboutX(45.0f);
			{
				Test::SceneTestFixture fixture(1, true);
				Scene& scene = fixture.GetScene();
				Test::InMemoryAssetManager assets;
				PublishRig(assets);
				Entity warden = scene.CreateEntity("Warden");
				SetPose(warden, glm::vec3(3.0f, 0.0f, -1.0f), AboutY(30.0f));
				warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(WalkHandle), MakeLayer(SwingHandle) }, AnimatorRootMotion::Apply));
				Entity staff = AddAttachment(scene, "Staff", warden, "Hand", offset, rotation);
				TransformSystem::Update(scene);
				Scope<AnimationSystem> system = CreateSystem(scene, assets);
				for (uint64_t tick = 0; tick < 90; ++tick)
				{
					CAPTURE(tick);
					StepAndUpdate(*system, scene, tick);
					CheckOnBone(staff, warden, "Hand", offset, rotation);
					CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
				}
				// The walk moved the Warden by root motion, and the staff with it.
				CHECK(warden.GetComponent<TransformComponent>().Translation != glm::vec3(3.0f, 0.0f, -1.0f));
				CHECK(system->GetDiagnostics().empty());
			}

			Test::SceneTestFixture fixture;
			Scene& edit = fixture.GetScene();
			static_cast<void>(Test::AddGround(edit));
			Entity rig = edit.CreateEntity("Rig");
			SetPose(rig, glm::vec3(0.0f, 1.0f, 0.0f));
			rig.AddComponent<CharacterControllerComponent>();
			Entity warden = edit.CreateEntity("Warden", rig);
			SetPose(warden, glm::vec3(0.0f, -0.9f, 0.0f), AboutY(-20.0f));
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(WalkHandle), MakeLayer(SwingHandle) }, AnimatorRootMotion::Apply));
			static_cast<void>(AddAttachment(edit, "Staff", warden, "Hand", offset, rotation));
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			CharacterDriver driver;
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, PlayMode::Play);
			specification.Assets = &assets;
			specification.Observer = &driver;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			Scene& scene = (*session)->GetScene();
			const Entity playedWarden = scene.FindEntityByPath("/Rig/Warden");
			const Entity playedStaff = scene.FindEntityByPath("/Rig/Warden/Staff");
			REQUIRE(playedStaff.IsValid());
			for (uint32_t tick = 0; tick < 60; ++tick)
			{
				CAPTURE(tick);
				(*session)->Tick();
				CheckOnBone(playedStaff, playedWarden, "Hand", offset, rotation);
			}
			// The character moved about 1.5 m along X under the walk.
			CHECK(Test::GetWorldPosition(**session, "/Rig").x > 1.0f);
		}

		// Canonical order visits parents first, so an attachment below another attachment is placed through its parent's
		// new Transform of the same pass: a charm on the staff attached to the hips (its Animator is still the Warden's) sits
		// on the hips exactly at every tick, not one tick of the staff's swing behind. A rider holding its own Animator
		// attaches to its mount's Animator above it, and the rider's lance to the rider's own hand.
		TEST_CASE("BoneAttachments: an attachment below another attachment follows its parent's new pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(WalkHandle), MakeLayer(SwingHandle) }));
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f), AboutX(30.0f));
			Entity charm = AddAttachment(scene, "Charm", staff, "Hips", glm::vec3(0.25f, 0.0f, 0.0f));
			Entity rider = AddAttachment(scene, "Rider", warden, "Hips", glm::vec3(0.0f, 0.3f, 0.0f));
			rider.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle, 0.5f) }, AnimatorRootMotion::None, TallSkeletonHandle));
			Entity lance = AddAttachment(scene, "Lance", rider, "Hand", glm::vec3(0.0f, 0.0f, 1.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			for (uint64_t tick = 0; tick < 45; ++tick)
			{
				CAPTURE(tick);
				StepAndUpdate(*system, scene, tick);
				CheckOnBone(charm, warden, "Hips", glm::vec3(0.25f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				CheckOnBone(rider, warden, "Hips", glm::vec3(0.0f, 0.3f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				CheckOnBone(lance, rider, "Hand", glm::vec3(0.0f, 0.0f, 1.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			}
			const BoneAttachmentStateComponent* riderState = rider.TryGetComponent<BoneAttachmentStateComponent>();
			REQUIRE(riderState != nullptr);
			CHECK(riderState->Animator == warden.GetUUID());
			CHECK(riderState->Joint == 0);
			const BoneAttachmentStateComponent* lanceState = lance.TryGetComponent<BoneAttachmentStateComponent>();
			REQUIRE(lanceState != nullptr);
			CHECK(lanceState->Animator == rider.GetUUID());
			CHECK(lanceState->Joint == 1);
		}

		// Not attached: no Animator above the entity (an entity's own Animator never counts), an empty Bone, a bone the
		// Skeleton lacks (the finding names the close joint names), an Animator whose Skeleton does not load, and one posed to
		// values that are not finite. Each keeps its Transform, holds no attachment state and is reported once with
		// ANIMATION_ATTACHMENT_INVALID, however many ticks run.
		TEST_CASE("BoneAttachments: an entity without a valid bone keeps its Transform and is reported")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
			Entity loose = AddAttachment(scene, "Loose", Entity{}, "Hand");
			loose.AddComponent<AnimatorComponent>(MakeAnimator({}));
			Entity empty = AddAttachment(scene, "Empty", warden, "");
			Entity typo = AddAttachment(scene, "Typo", warden, "Hnad");
			Entity statue = scene.CreateEntity("Statue");
			statue.AddComponent<AnimatorComponent>(MakeAnimator({}, AnimatorRootMotion::None, AssetHandle(0x404)));
			Entity unposed = AddAttachment(scene, "Unposed", statue, "Hand");
			Entity broken = scene.CreateEntity("Broken");
			broken.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(OverflowHandle, 0.5f) }));
			Entity overflowed = AddAttachment(scene, "Overflowed", broken, "Hand");
			const std::vector<Entity> invalid = { loose, empty, typo, unposed, overflowed };
			for (const Entity entity : invalid)
				SetPose(entity, glm::vec3(1.0f, 2.0f, 3.0f), AboutY(10.0f));
			TransformSystem::Update(scene);

			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			const Test::ExpectLog findings(LogLevel::Warn, AnimationAttachmentInvalidCode);
			for (uint64_t tick = 0; tick < 5; ++tick)
				StepAndUpdate(*system, scene, tick);
			for (const Entity entity : invalid)
			{
				CAPTURE(entity.GetName());
				const TransformComponent& transform = entity.GetComponent<TransformComponent>();
				CHECK(transform.Translation == glm::vec3(1.0f, 2.0f, 3.0f));
				CHECK(transform.Rotation == AboutY(10.0f));
				CHECK_FALSE(entity.HasComponent<BoneAttachmentStateComponent>());
				CHECK(CountFindings(system->GetDiagnostics(), entity.GetUUID()) == 1);
			}
			CHECK(findings.GetMatchCount() == invalid.size());
			const AnimationSceneIssue* misspelt = FindIssue(system->GetDiagnostics(), typo.GetUUID());
			REQUIRE(misspelt != nullptr);
			CHECK(misspelt->Component == "BoneAttachment");
			CHECK(misspelt->Field == "Bone");
			CHECK(misspelt->Severity == DiagnosticSeverity::Warning);
			CHECK(misspelt->Message.find("'Hnad'") != std::string::npos);
			CHECK(misspelt->Message.find("Did you mean 'Hand'?") != std::string::npos);
			const AnimationSceneIssue* nameless = FindIssue(system->GetDiagnostics(), empty.GetUUID());
			REQUIRE(nameless != nullptr);
			CHECK(nameless->Field == "Bone");
			CHECK(nameless->Hint.find("'Hips' and 'Hand'") != std::string::npos);
			CHECK(FindIssue(system->GetDiagnostics(), loose.GetUUID())->Field.empty());
		}

		// A script can spawn a broken attachment every tick, so the pass reports at most MaxAnimationSystemDiagnostics entities,
		// the most the AnimationSystem keeps: past them nothing more is kept or logged, and one warning says so.
		TEST_CASE("BoneAttachments: findings past MaxAnimationSystemDiagnostics are neither kept nor logged")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			for (size_t index = 0; index < MaxAnimationSystemDiagnostics + 1; ++index)
				static_cast<void>(AddAttachment(scene, "Loose", Entity{}, "Hand"));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			const Test::ExpectLog findings(LogLevel::Warn, AnimationAttachmentInvalidCode);
			const Test::ExpectLog reached(LogLevel::Warn, "animation findings reached");
			StepAndUpdate(*system, scene, 0);
			static_cast<void>(AddAttachment(scene, "Late", Entity{}, "Hand"));
			StepAndUpdate(*system, scene, 1);
			CHECK(system->GetDiagnostics().size() == MaxAnimationSystemDiagnostics);
			CHECK(findings.GetMatchCount() == MaxAnimationSystemDiagnostics);
			CHECK(reached.GetMatchCount() == 1);
		}

		// The animation checks of project.validate (FindBoneAttachmentIssues, edit scenes): one finding per entity, the first
		// cause in order, each warning not attached or fighting its placement: no Animator above, an empty Bone, a joint the
		// loaded Skeleton lacks, a CharacterController, a Dynamic RigidBody, solid colliders without a Kinematic body of its
		// own (its own collider, or a child's that the entity's static body would gather). A Kinematic body with a solid
		// collider, a trigger without a body, a Skeleton that does not load and a disabled entity are not reported.
		TEST_CASE("BoneAttachments: FindBoneAttachmentIssues reports the first cause of each attachment that cannot attach or fights its placement")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({}));
			Entity loose = AddAttachment(scene, "Loose", Entity{}, "Hand");
			Entity empty = AddAttachment(scene, "Empty", warden, "");
			Entity typo = AddAttachment(scene, "Typo", warden, "hand");
			Entity character = AddAttachment(scene, "Character", warden, "Hand");
			character.AddComponent<CharacterControllerComponent>();
			Entity dynamic = AddAttachment(scene, "Dynamic", warden, "Hand");
			dynamic.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Dynamic });
			dynamic.AddComponent<SphereColliderComponent>();
			Entity bare = AddAttachment(scene, "Bare", warden, "Hand");
			bare.AddComponent<BoxColliderComponent>();
			Entity holder = AddAttachment(scene, "Holder", warden, "Hand");
			holder.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .IsTrigger = true });
			scene.CreateEntity("Blade", holder).AddComponent<BoxColliderComponent>();
			Entity swept = AddAttachment(scene, "Swept", warden, "Hand");
			swept.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Kinematic });
			swept.AddComponent<BoxColliderComponent>();
			scene.CreateEntity("Edge", swept).AddComponent<BoxColliderComponent>();
			Entity trigger = AddAttachment(scene, "Trigger", warden, "Hand");
			trigger.AddComponent<SphereColliderComponent>(SphereColliderComponent{ .IsTrigger = true });
			Entity statue = scene.CreateEntity("Statue");
			statue.AddComponent<AnimatorComponent>(MakeAnimator({}, AnimatorRootMotion::None, AssetHandle(0x404)));
			static_cast<void>(AddAttachment(scene, "Unloaded", statue, "Anything"));
			Entity off = AddAttachment(scene, "Off", Entity{}, "Hand");
			off.SetActive(false);

			const std::vector<AnimationSceneIssue> issues = FindBoneAttachmentIssues(scene, assets);
			REQUIRE(issues.size() == 7);
			// Canonical order: the Warden's children, then the roots after it.
			const std::vector<Entity> reported = { empty, typo, character, dynamic, bare, holder, loose };
			for (size_t index = 0; index < reported.size(); ++index)
			{
				CAPTURE(reported[index].GetName());
				CHECK(issues[index].Entity == reported[index].GetUUID());
				CHECK(issues[index].Code == AnimationAttachmentInvalidCode);
				CHECK(issues[index].Severity == DiagnosticSeverity::Warning);
				CHECK(issues[index].Component == "BoneAttachment");
				CHECK_FALSE(issues[index].Message.empty());
				CHECK_FALSE(issues[index].Hint.empty());
			}
			CHECK(issues[0].Field == "Bone");
			CHECK(issues[1].Field == "Bone");
			CHECK(issues[1].Message.find("Did you mean 'Hand'?") != std::string::npos);
			CHECK(issues[2].Message.find("CharacterController") != std::string::npos);
			CHECK(issues[3].Message.find("Dynamic RigidBody") != std::string::npos);
			CHECK(issues[4].Message.find("Kinematic body") != std::string::npos);
			CHECK(issues[5].Message.find("Kinematic body") != std::string::npos);
			CHECK(issues[6].Field.empty());
			CHECK(FindBoneAttachmentIssues(*fixture.CreateEmptyScene(), assets).empty());
		}

		// While the Animator's pose holds (Playing false: the scrubbed pose), the pass writes nothing: the scene's revision
		// stays, though the attachment stays attached. Writing a layer's Time scrubs the pose, and the attachment follows.
		TEST_CASE("BoneAttachments: a held pose changes no revision")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			AnimatorComponent animator = MakeAnimator({ MakeLayer(SwingHandle, 0.25f) });
			animator.Playing = false;
			warden.AddComponent<AnimatorComponent>(animator);
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			StepAndUpdate(*system, scene, 0);
			CheckOnBone(staff, warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			const glm::vec3 scrubbed = staff.GetComponent<TransformComponent>().Translation;
			const uint64_t revision = scene.GetRevision();
			for (uint64_t tick = 1; tick < 10; ++tick)
				StepAndUpdate(*system, scene, tick);
			CHECK(scene.GetRevision() == revision);
			CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
			CHECK(SameBits(staff.GetComponent<TransformComponent>().Translation, scrubbed));

			warden.Patch<AnimatorComponent>([](AnimatorComponent& component)
			{
				component.Layers[0].Time = 0.5f;
			});
			StepAndUpdate(*system, scene, 10);
			CheckOnBone(staff, warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(staff.GetComponent<TransformComponent>().Translation != scrubbed);
		}

		// The animation events reach the listener after the attaching pass: a callback reads the attached entity at the pose
		// of the tick that fired the event, never the previous tick's.
		TEST_CASE("BoneAttachments: an animation event callback reads the placed pose")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.0f, 1.0f, 0.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			PlacementListener listener(scene, "/Warden/Staff");
			system->SetEventListener(&listener);
			for (uint64_t tick = 0; tick < 30 && listener.Events.empty(); ++tick)
				StepAndUpdate(*system, scene, tick);
			REQUIRE(listener.Events.size() == 1);
			CHECK(listener.Events[0].Name == "Strike");
			// The Transform the callback read is the one the tick placed.
			CHECK(SameBits(listener.Translations[0], staff.GetComponent<TransformComponent>().Translation));
			CheckOnBone(staff, warden, "Hand", glm::vec3(0.0f, 1.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		// Removing the Animator component or reparenting the attached entity away detaches it (its Transform kept, a
		// finding); under another Animator it attaches to that Skeleton's joint of the same name; disabling the Animator's
		// entity disables the attachment with it.
		TEST_CASE("BoneAttachments: removing the Animator or reparenting away detaches, and another Animator's joint of the same name attaches")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
			Entity giant = scene.CreateEntity("Giant");
			SetPose(giant, glm::vec3(5.0f, 0.0f, 0.0f));
			giant.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }, AnimatorRootMotion::None, TallSkeletonHandle));
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand");
			Entity torch = AddAttachment(scene, "Torch", warden, "Hand");
			Entity shield = AddAttachment(scene, "Shield", giant, "Hand");
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			for (uint64_t tick = 0; tick < 3; ++tick)
				StepAndUpdate(*system, scene, tick);

			// The staff is handed to the giant: under its Animator, on its own "Hand".
			REQUIRE(scene.SetParent(staff, giant, std::nullopt, false).has_value());
			// The torch is dropped into the world: detached, where it is.
			REQUIRE(scene.SetParent(torch, Entity{}, std::nullopt, true).has_value());
			const TransformComponent dropped = torch.GetComponent<TransformComponent>();
			StepAndUpdate(*system, scene, 3);
			CheckOnBone(staff, giant, "Hand", glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			CHECK(staff.GetComponent<BoneAttachmentStateComponent>().Animator == giant.GetUUID());
			CHECK_FALSE(torch.HasComponent<BoneAttachmentStateComponent>());
			CHECK(SameBits(torch.GetComponent<TransformComponent>().Translation, dropped.Translation));
			CHECK(CountFindings(system->GetDiagnostics(), torch.GetUUID()) == 1);

			// The giant loses its Animator: the shield and the staff keep their Transforms, detached.
			const TransformComponent held = shield.GetComponent<TransformComponent>();
			giant.RemoveComponent<AnimatorComponent>();
			StepAndUpdate(*system, scene, 4);
			CHECK_FALSE(shield.HasComponent<BoneAttachmentStateComponent>());
			CHECK_FALSE(staff.HasComponent<BoneAttachmentStateComponent>());
			CHECK(SameBits(shield.GetComponent<TransformComponent>().Translation, held.Translation));
			CHECK(CountFindings(system->GetDiagnostics(), shield.GetUUID()) == 1);

			// A disabled Animator's entity disables its attachments with it; enabled again, they are placed at once.
			Entity bow = AddAttachment(scene, "Bow", warden, "Hand", glm::vec3(0.0f, 0.0f, 0.5f));
			StepAndUpdate(*system, scene, 5);
			REQUIRE(bow.HasComponent<BoneAttachmentStateComponent>());
			warden.SetActive(false);
			StepAndUpdate(*system, scene, 6);
			CHECK_FALSE(bow.HasComponent<BoneAttachmentStateComponent>());
			warden.SetActive(true);
			StepAndUpdate(*system, scene, 7);
			CHECK(bow.HasComponent<BoneAttachmentStateComponent>());
			CheckOnBone(bow, warden, "Hand", glm::vec3(0.0f, 0.0f, 0.5f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
		}

		// A disabled attached entity is skipped (no state, its Transform held while the hand moves) and placed again at the
		// first phase after it is enabled; its own Transform writes (a script's, automation's, a teleport) are overwritten
		// at the next phase, while writes to Offset and Rotation take effect there.
		TEST_CASE("BoneAttachments: a disabled attachment is skipped and placed again once enabled, and its own Transform writes are overwritten")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			StepAndUpdate(*system, scene, 0);

			staff.SetActive(false);
			const glm::vec3 stowed = staff.GetComponent<TransformComponent>().Translation;
			for (uint64_t tick = 1; tick < 10; ++tick)
				StepAndUpdate(*system, scene, tick);
			CHECK_FALSE(staff.HasComponent<BoneAttachmentStateComponent>());
			CHECK(SameBits(staff.GetComponent<TransformComponent>().Translation, stowed));
			staff.SetActive(true);
			StepAndUpdate(*system, scene, 10);
			CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
			CheckOnBone(staff, warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));

			SetPose(staff, glm::vec3(40.0f, 0.0f, 0.0f), AboutX(90.0f));
			staff.Patch<BoneAttachmentComponent>([](BoneAttachmentComponent& attachment)
			{
				attachment.Offset = glm::vec3(0.0f, 0.0f, -0.5f);
				attachment.Rotation = AboutY(90.0f);
			});
			StepAndUpdate(*system, scene, 11);
			CheckOnBone(staff, warden, "Hand", glm::vec3(0.0f, 0.0f, -0.5f), AboutY(90.0f));
			CHECK(system->GetDiagnostics().empty());
		}

		// The Animator entity's scale, non-uniform included, reaches the attachment through the hierarchy; the bone's scale
		// is ignored (a hand grown to 3 holds a staff of its own scale at its unscaled offset), and a bone scaled to nothing
		// leaves the Transform as it is, still attached. Under a non-uniformly scaled entity between the Animator's entity and
		// the attachment, the placement takes DecomposeMatrix's closest rotation and keeps the position exact.
		TEST_CASE("BoneAttachments: the Animator's scale reaches the attachment and the bone's scale is ignored")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			SetPose(warden, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(2.0f, 1.0f, 1.0f));
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(GrowHandle) }));
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.0f, 1.0f, 0.0f));
			SetPose(staff, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f), glm::vec3(0.5f));
			Entity sling = scene.CreateEntity("Sling", warden);
			SetPose(sling, glm::vec3(0.1f, 0.2f, 0.3f), AboutY(25.0f), glm::vec3(1.0f, 3.0f, 1.0f));
			Entity pouch = AddAttachment(scene, "Pouch", sling, "Hips", glm::vec3(0.5f, 0.0f, 0.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);
			StepAndUpdate(*system, scene, 0);

			// In the Warden's space: the hand at (0, 2, 0), the offset unscaled by the hand's 3.
			const TransformComponent& held = staff.GetComponent<TransformComponent>();
			CHECK(Test::ApproxEqual(held.Translation, glm::vec3(0.0f, 3.0f, 0.0f)));
			CHECK(held.Scale == glm::vec3(0.5f));
			// In the world, the Warden's X scale of 2 reaches it.
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldPosition(staff), glm::vec3(0.0f, 3.0f, 0.0f)));
			CHECK(Test::ApproxEqual(TransformSystem::GetWorldScale(staff), glm::vec3(1.0f, 0.5f, 0.5f)));
			// Through the sling's non-uniform scale the position is still the hips' plus the offset in the Warden's space.
			const glm::vec3 pouchInWarden(glm::inverse(TransformSystem::ComputeWorldMatrix(warden)) * glm::vec4(TransformSystem::GetWorldPosition(pouch), 1.0f));
			CHECK(Test::ApproxEqual(pouchInWarden, glm::vec3(0.5f, 1.0f, 0.0f)));
			CHECK(pouch.HasComponent<BoneAttachmentStateComponent>());

			// The hand scaled to nothing: the staff stays where it was, attached.
			warden.Patch<AnimatorComponent>([](AnimatorComponent& animator)
			{
				animator.Layers[0].Clip = TypedAssetHandle<AssetType::AnimationClip>(VanishHandle);
			});
			const glm::vec3 before = staff.GetComponent<TransformComponent>().Translation;
			StepAndUpdate(*system, scene, 1);
			CHECK(SameBits(staff.GetComponent<TransformComponent>().Translation, before));
			CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
			CHECK(system->GetDiagnostics().empty());
		}

		// The world pause holds the phase, so an attachment holds with its pose and its Transform until the world resumes;
		// Simulate steps the phase, so it places attachments as Play does.
		TEST_CASE("BoneAttachments: the world pause holds an attachment, and Simulate places it")
		{
			for (const PlayMode mode : { PlayMode::Play, PlayMode::Simulate })
			{
				CAPTURE(static_cast<int>(mode));
				Test::SceneTestFixture fixture;
				Entity warden = fixture.GetScene().CreateEntity("Warden");
				warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
				static_cast<void>(AddAttachment(fixture.GetScene(), "Staff", warden, "Hand", glm::vec3(0.0f, 0.5f, 0.0f)));
				Test::InMemoryAssetManager assets;
				PublishRig(assets);
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, mode);
				specification.Assets = &assets;
				Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
				REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
				Scene& scene = (*session)->GetScene();
				const Entity staff = scene.FindEntityByPath("/Warden/Staff");
				Test::RunTicks(**session, 10);
				CheckOnBone(staff, scene.FindEntityByPath("/Warden"), "Hand", glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
				CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
				if (mode == PlayMode::Simulate)
					continue;

				REQUIRE((*session)->RequestWorldPause(true).has_value());
				Test::RunTicks(**session, 1);
				const glm::vec3 paused = staff.GetComponent<TransformComponent>().Translation;
				Test::RunTicks(**session, 10);
				CHECK(SameBits(staff.GetComponent<TransformComponent>().Translation, paused));
				CHECK(staff.HasComponent<BoneAttachmentStateComponent>());
				REQUIRE((*session)->RequestWorldPause(false).has_value());
				Test::RunTicks(**session, 3);
				CHECK(staff.GetComponent<TransformComponent>().Translation != paused);
				CheckOnBone(staff, scene.FindEntityByPath("/Warden"), "Hand", glm::vec3(0.0f, 0.5f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			}
		}

		// An attached Kinematic body is placed before PhysicsPreStep, which sweeps it to its new pose with MoveKinematic: its
		// velocity is the bone's motion over the step, so a swinging staff pushes what it meets.
		TEST_CASE("BoneAttachments: an attached Kinematic body is swept with its bone")
		{
			Test::SceneTestFixture fixture;
			Entity warden = fixture.GetScene().CreateEntity("Warden");
			warden.AddComponent<AnimatorComponent>(MakeAnimator({ MakeLayer(SwingHandle) }));
			Entity staff = AddAttachment(fixture.GetScene(), "Staff", warden, "Hand", glm::vec3(0.0f, 1.0f, 0.0f));
			staff.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Kinematic });
			staff.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(0.05f, 1.0f, 0.05f) });
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, 42, PlayMode::Play);
			specification.Assets = &assets;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			const UUID id = Test::GetEntityId((*session)->GetScene(), "/Warden/Staff");
			Test::RunTicks(**session, 5);
			for (uint32_t tick = 0; tick < 20; ++tick)
			{
				CAPTURE(tick);
				const glm::vec3 before = Test::GetWorldPosition(**session, "/Warden/Staff");
				(*session)->Tick();
				const glm::vec3 after = Test::GetWorldPosition(**session, "/Warden/Staff");
				const Result<glm::vec3> velocity = (*session)->GetPhysics().GetLinearVelocity(id);
				REQUIRE_MESSAGE(velocity.has_value(), velocity.error().ToString());
				const glm::vec3 expected = (after - before) / static_cast<float>((*session)->GetFixedDelta());
				CHECK(glm::length(expected) > 0.5f);
				CHECK(Test::ApproxEqual(*velocity, expected, 1e-3f));
			}
			const std::optional<PhysicsBodyReport> body = (*session)->GetPhysics().GetBodyInfo(id);
			REQUIRE(body.has_value());
			CHECK((*session)->GetAnimation()->GetDiagnostics().empty());
		}

		// Determinism (decision 5): 600 ticks of a clip that walks with root motion Apply and swings (slerped through DetMath),
		// with an attachment on the hand (Offset and Rotation), one below it on the hips and one under a non-uniformly scaled
		// entity (DecomposeMatrix's closest rotation), hash every tick's attached Transforms to one committed value, in Debug
		// and Release alike.
		TEST_CASE("BoneAttachments: a clip stepped 600 ticks ends at its committed hash")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			PublishRig(assets);
			Entity warden = scene.CreateEntity("Warden");
			SetPose(warden, glm::vec3(1.0f, 0.0f, 2.0f), AboutY(37.0f));
			AnimatorComponent animator = MakeAnimator({ MakeLayer(WalkHandle), MakeLayer(SwingHandle, 0.125f) }, AnimatorRootMotion::Apply);
			animator.Layers[1].Speed = 1.3f;
			warden.AddComponent<AnimatorComponent>(animator);
			Entity staff = AddAttachment(scene, "Staff", warden, "Hand", glm::vec3(0.05f, 0.6f, -0.02f), AboutX(-75.0f));
			Entity charm = AddAttachment(scene, "Charm", staff, "Hips", glm::vec3(0.0f, 0.1f, 0.2f), AboutY(15.0f));
			Entity sling = scene.CreateEntity("Sling", warden);
			SetPose(sling, glm::vec3(0.2f, 0.1f, 0.0f), AboutX(12.0f), glm::vec3(1.0f, 1.5f, 0.75f));
			Entity pouch = AddAttachment(scene, "Pouch", sling, "Hand", glm::vec3(0.1f, 0.0f, 0.0f), AboutY(-40.0f));
			TransformSystem::Update(scene);
			Scope<AnimationSystem> system = CreateSystem(scene, assets);

			XXH64Hasher hasher;
			for (uint64_t tick = 0; tick < 600; ++tick)
			{
				StepAndUpdate(*system, scene, tick);
				for (const Entity entity : { staff, charm, pouch })
				{
					const TransformComponent& transform = entity.GetComponent<TransformComponent>();
					hasher.Update(std::as_bytes(std::span(&transform.Translation.x, 3)));
					const std::array<float, 4> rotation = { transform.Rotation.x, transform.Rotation.y, transform.Rotation.z, transform.Rotation.w };
					hasher.Update(std::as_bytes(std::span(rotation)));
				}
			}
			CHECK(system->GetDiagnostics().empty());
			CHECK(std::format("{:016x}", hasher.Digest()) == "a709a18bf99f3fbf");
		}
	}

}
