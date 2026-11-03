#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/AnimationValidation.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

// Skeletal and morph animation in a scene (M23, C23.3, C-SKEL, C-MORPH, C-ANIM-BLEND; Docs/Decisions/0034-m23-contract.md
// decisions 29 and 30): the AnimationSystem phase of the fixed step (PlaySessionPhase::AnimationSystem, after NativeStep, and
// after M21's SpriteAnimation on the merged tree, before PreStepTransformUpdate; ADR 0028's phase-2 execution order), in Play
// and Simulate alike. It runs after the tick's gameplay code (scripts, tasks, native systems), which starts clips and sets
// weights, so a clip started in OnFixedUpdate is sampled at that tick; one started in a contact callback or OnDestroy (later
// in the tick) advances from the next tick. Root motion it applies reaches physics through PreStepTransformUpdate, as script
// and native writes do.
//
// The step has two passes, as PhysicsSystem's PostStep steps before it dispatches (decision 29):
//   Stepping, for every entity with an Animator in canonical order (the order taken once at the phase's start), skipping an
//   entity that is pending destruction (PendingDestroyTag, destroyed earlier in the tick) or effectively disabled, whose pose
//   component stays unchanged:
//     1. With Playing, each layer whose clip loads advances its Time (AdvanceAnimationTime by step.FixedDelta * Speed, Loop),
//        written back to the Animator through the scene, so the change tracker and the state hash see it, and the events the
//        advance passes are collected (CollectAnimationEvents), at most MaxAnimationEventsPerStep for the Animator in (layer,
//        firing) order: the rest are dropped, and an Animator's first drop logs one warning for the system's lifetime. A
//        layer whose clip is null or fails to load keeps its Time.
//     2. When the Skeleton loads (through the session's AssetManager), the pose: the rest pose, its morph lists seeded with
//        the SkinnedMesh weights of each Target a loaded layer's clip keys (BlendAnimationPose's base), then each layer in
//        order whose clip loads and whose Weight is above 0, sampled at its Time and blended by its Weight
//        (Animation/AnimationPose.h); with RootMotion Apply or Extract, each such layer's root motion is computed over its
//        step and held out of its pose (Animation/RootMotion.h), and the layers' deltas are blended in order by
//        BlendRootMotion, which blends only the root paths (the root-motion joint's translation and rotation) the layer's
//        clip keys, as poses blend only keyed paths: a layer whose clip keys neither contributes nothing. The pose's joint
//        model and skin matrices, morph weights and root motion are written to the entity's AnimationPoseComponent (added
//        when missing), with the tick. A skeleton that is null or fails to load removes the component, and nothing below is
//        drawn deformed.
//     3. With RootMotion Apply, the entity's Transform moves by the blended delta: Translation += Rotation * (Scale * delta
//        translation), Rotation = Rotation * delta rotation (normalized), through the scene.
//   Dispatch, after every Animator was stepped: the collected events, in (canonical entity, layer, firing) order, reach the
//   listener (SetEventListener: the session's ScriptEngine in Play; none in Simulate) as AnimationEventRecords. So every
//   callback reads every Animator's new pose, and what a callback changes (a clip started, an Animator destroyed, disabled
//   or created) takes effect from the next tick whatever the entities' order. An event whose entity an earlier callback of
//   the dispatch destroyed (pending destruction) or disabled is dropped, as physics drops a callback whose Self is gone; the
//   dispatch walks its own list, so a callback's Scene.CreateEntity never invalidates it.
// An entity without an Animator is never touched: a scene without one adds no runtime component, writes nothing and hashes as
// before (decision 29's live guard).
//
// Threading: main thread only. Not copyable or movable. Frozen by the M23 contract C23.3; stream B implements it.

namespace Engine {

	class AssetManager;
	class ConstEntity;
	class Entity;
	class Scene;

	// The most findings a system keeps (GetDiagnostics), since a script can write a new clip every tick: past it a finding is
	// neither kept nor logged, and one warning says so.
	inline constexpr size_t MaxAnimationSystemDiagnostics = 1024;
	// The most events one Animator delivers in one step (the file comment's step 1), so a clip of many events looping many
	// times a step (an imported file controls both) cannot flood the listener: the events past it are dropped, and the
	// Animator's first drop logs one Warn line naming its entity's id (UUID::ToString), never again for that entity
	// (decisions 26 and 29).
	inline constexpr uint32_t MaxAnimationEventsPerStep = 256;

	// One animation event of a fixed step (CollectAnimationEvents' order within a layer).
	struct AnimationEventRecord
	{
		UUID Entity{};      // the Animator's entity
		uint32_t Layer = 0; // the layer's index in Layers
		AssetHandle Clip{}; // the layer's clip
		std::string Name{}; // AnimationEvent::Name
		float Time = 0.0f;  // AnimationEvent::Time, seconds into the clip
		uint64_t Tick = 0;  // the fixed step

		bool operator==(const AnimationEventRecord&) const = default;
	};

	// The receiver of a step's animation events: the play session's ScriptEngine, which calls OnAnimationEvent on every slot of
	// the event's entity in slot order (ADR 0031 decision 17), as it dispatches physics callbacks.
	class IAnimationEventListener
	{
	public:
		virtual ~IAnimationEventListener() = default;

		virtual void OnAnimationEvent(const AnimationEventRecord& event) = 0;
	};

	// The receiver of the events is not part of the specification: the play session creates the system before its
	// ScriptEngine and replaces that engine at a scene transition, so it sets the receiver with SetEventListener.
	struct AnimationSystemSpecification
	{
		// Required, outliving the system (documented back-references): the play session's runtime scene and asset manager.
		Scene* RuntimeScene = nullptr;
		AssetManager* Assets = nullptr;
	};

	class AnimationSystem
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class AnimationSystem;
		};

		// Use Create.
		explicit AnimationSystem(ConstructionKey key);
		~AnimationSystem();

		AnimationSystem(const AnimationSystem&) = delete;
		AnimationSystem& operator=(const AnimationSystem&) = delete;

		// Errors: InvalidArgument for a missing scene or asset manager.
		[[nodiscard]] static Result<Scope<AnimationSystem>> Create(const AnimationSystemSpecification& specification);

		// The AnimationSystem phase (see the file comment) for `step` (its Tick and FixedDelta).
		void Step(const SimStep& step);
		// The listener the dispatch delivers to; null (the default) delivers to nobody, and the events are collected and
		// dropped (Simulate). Not owned; it must outlive the system or be replaced first. The play session sets its
		// ScriptEngine whenever it creates one (at its start, Scene.Load and Scene.Reload: PrepareScripts, beside
		// PhysicsSystem::SetEventListener) and clears it before that engine is destroyed (the transition and ~PlaySession,
		// before Scripts.reset()), so the system never delivers to a destroyed engine (decision 29). Never called from the
		// dispatch.
		void SetEventListener(IAnimationEventListener* listener);
		// The findings raised so far (FindAnimationSceneIssues' codes, raised once per entity and code at run time), in the
		// order raised.
		[[nodiscard]] std::span<const AnimationSceneIssue> GetDiagnostics() const;
	private:
		// The back-references, the diagnostics raised and the step's scratch poses (Scene/AnimationSystem.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// Writes the pose of the Animator on `animator` (valid, holding an Animator) into its AnimationPoseComponent (step 2 of
	// the file comment, Tick 0, no root motion and no events), each layer sampled at its Time advanced by `previewTime`
	// seconds (>= 0, finite) times its Speed, wrapped or held at its clip's end as AdvanceAnimationTime does (0: the layers'
	// times): the editor's preview of an edit scene (EditorCore/Animation/AnimationPreview.h). Writes nothing to the Animator,
	// whatever Playing says. Errors: NotFound when the Skeleton is null or fails to load (the component is removed), naming
	// the asset; InvalidArgument for a `previewTime` that is not finite or below 0.
	[[nodiscard]] Status PoseAnimator(Entity animator, AssetManager& assets, float previewTime = 0.0f);

	// The Animator that poses `entity`'s SkinnedMesh: `entity` itself or its nearest ancestor holding an Animator; an invalid
	// handle when there is none. Pure.
	[[nodiscard]] ConstEntity FindDrivingAnimator(ConstEntity entity);

	// The world transform of the joint named `joint` (FindSkeletonJoint) of `animator`'s last pose: the Animator entity's
	// world matrix (WorldTransformComponent, as the last transform update left it) times the joint's model transform;
	// nullopt when the entity holds no AnimationPoseComponent or its skeleton has no such joint. What Animator:GetJointTransform
	// reads in scripts: the pose of the last AnimationSystem phase, deterministic since the phase runs in the fixed step.
	// Pure.
	[[nodiscard]] std::optional<glm::mat4> FindAnimatorJointWorldTransform(ConstEntity animator, std::string_view joint);

}
