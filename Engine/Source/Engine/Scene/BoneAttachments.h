#pragma once

#include "Engine/Asset/Asset.h"
#include "Engine/Asset/SkeletonData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Scene/AnimationValidation.h"

#include <glm/glm.hpp>

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Bone attachments (M41, Docs/Decisions/0058-m41-bone-attachments.md decision 3; Architecture §5.2, §5.3 and §5.7): an
// entity with a BoneAttachment follows a joint of the Skeleton of the nearest Animator above it (a bone), placed by the
// engine once per fixed step and drawn with the skin.
//
// The Animator. FindDrivingAnimator started at the entity's parent: the parent, or its nearest ancestor that holds an
// Animator, never the entity's own (a rider holding its own Animator attaches to its mount's). It is always an ancestor, so
// an attachment can neither dangle nor form a cycle, and it is placed in that Animator entity's space, exact whatever moves
// the character later in the tick.
//
// The attaching pass (BoneAttachmentPass::Run), the AnimationSystem phase's third pass (Scene/AnimationSystem.h), after every
// Animator was stepped and posed (root motion Apply included) and before the animation events reach the scripts. It visits
// every entity with a BoneAttachment in canonical order, parents first, so an attachment below another attachment sees its
// parent's new pose. Each effectively enabled one whose Animator has a pose (AnimationPoseComponent) whose Skeleton holds the
// joint named Bone (matched exactly, as FindSkeletonJoint and Animator:GetJointTransform match it) is attached:
//   - From the joint's model transform J (in the Animator entity's space) decomposed by TransformSystem::DecomposeMatrix, its
//     scale ignored: P = translation(J) + rotation(J) * Offset and Q = normalize(rotation(J) * Rotation).
//   - P and Q go into the parent's space through the product M of the local matrices (TransformSystem::ComputeLocalMatrix)
//     of the entities from the Animator's entity (excluded) down to the parent (included), never through world matrices: a
//     direct child of the Animator's entity (M the identity) takes P and Q exactly; otherwise the entity takes the
//     translation and rotation of DecomposeMatrix(inverse(M) * T(P) * R(Q)), the closest rotation under a non-uniform scale.
//   - Only the Transform's Translation and Rotation are written, through the scene (Entity::Patch: the change tracker, the
//     revision and the state hash see them), and only when either differs bit for bit, so a pose that holds changes no
//     revision. Scale stays as authored.
//   - A joint matrix, chain or result that does not decompose or is not finite (a joint scaled to nothing, a zero scale on
//     the way) writes nothing that tick: the entity stays attached at its Transform.
//   - The entity gets BoneAttachmentStateComponent {Animator, Joint}; every other entity loses it, so it marks exactly the
//     entities the last pass attached.
// An entity that is not attached keeps its Transform and renders by plain interpolation: no Animator above it, an empty Bone,
// an Animator without a pose (its Skeleton null, not loading, or posed to values that are not finite: AnimationSystem.h,
// step 2), or a Skeleton without the joint. Each is reported once per entity (ANIMATION_ATTACHMENT_INVALID, below), and so
// is an attached entity whose own physics fights the placement. A disabled entity is skipped and attached again at the first
// pass after it is enabled. The pass runs in Play and Simulate and not under the world pause (the phase does not run), where
// attachments hold with the poses; nothing runs it in edit scenes or for the editor's animation preview.
//
// Determinism: IEEE-exact operations, sqrt and DetMath only (the simulation path), over the pose (a pure function of the
// Animator's fields, the assets and the tick) and the scene's Transforms, in canonical order: the written Transform is
// hashed state like any other, as root motion Apply's is.
//
// Rendering (decision R; Scene/RenderExtraction.h's "Attached subtrees" and TransformSystem::GetRenderPosition and
// GetRenderRotation): an entity at or below an attached entity renders at its Animator's rendered pose times its current pose
// relative to that Animator's entity (ComputeAttachedRelativeMatrix), exactly as the skin's palette does, through
// FindAttachedRenderAnimator. A scene without BoneAttachmentStateComponent takes the plain rule (one view emptiness check).
//
// Threading: main thread only, like the scene.

namespace Engine {

	class AssetManager;
	class ConstEntity;
	class Scene;

	// Warning, not fixable: an entity's BoneAttachment cannot attach it, or its physics fights the attachment: no Animator
	// above the entity, an empty Bone, a Skeleton (that loads) without the joint named Bone (with up to three close joint
	// names), at run time an Animator without a pose, a CharacterController or a Dynamic RigidBody on the entity, or solid
	// colliders that move with it and form no Kinematic body of its own (an ancestor's compound rebuilt every step, a static
	// body teleported every step, or, under a CharacterController, no body at all, so they collide with nothing). Appended after M37's PHYSICS_JOINT_INVALID in both of ProjectValidator.cpp's
	// tables, an array of its own (BoneAttachmentDiagnosticCodes; the frozen AnimationValidation.h is M23's).
	inline constexpr std::string_view AnimationAttachmentInvalidCode = "ANIMATION_ATTACHMENT_INVALID";
	inline constexpr std::array<std::string_view, 1> BoneAttachmentDiagnosticCodes = { AnimationAttachmentInvalidCode };

	// A runtime-only component (Components/RuntimeComponents.h's rules: never registered, serialized or hashed, invisible to
	// scripts and automation, and adding or removing it changes neither the scene's revision nor the change tracker): the
	// attachment the last attaching pass made. Render extraction and the scripts' render pose read it (decision R).
	struct BoneAttachmentStateComponent
	{
		UUID Animator{};    // the Animator's entity, an ancestor of the attached entity when the pass ran
		uint32_t Joint = 0; // the joint's index in that Animator's Skeleton
	};

	// The findings of `scene`'s BoneAttachments (effectively enabled entities, canonical order), at most one per entity, the
	// first cause in this order: no Animator above it; an empty Bone; the Skeleton of its Animator, loaded through `assets`,
	// without the joint (one that does not load is reported by the asset checks; a null one only in play, as an Animator
	// without a pose); a CharacterController;
	// a Dynamic RigidBody; solid colliders without a Kinematic RigidBody of its own. Each an AnimationSceneIssue on the
	// entity, Component "BoneAttachment" (Field "Bone" for the bone's causes), Severity Warning. What project.validate reports.
	// Pure; main thread.
	[[nodiscard]] std::vector<AnimationSceneIssue> FindBoneAttachmentIssues(const Scene& scene, AssetManager& assets);

	// The attaching pass and what it keeps across ticks (owned by the AnimationSystem): the joint indices found per Skeleton
	// and name, and the entities whose finding was reported. Not copyable.
	class BoneAttachmentPass
	{
	public:
		BoneAttachmentPass() = default;
		BoneAttachmentPass(const BoneAttachmentPass&) = delete;
		BoneAttachmentPass& operator=(const BoneAttachmentPass&) = delete;

		// The pass over `scene` (a runtime scene) as the file comment describes. The finding of each entity not reported
		// before is appended to `findings` (the AnimationSystem raises it), at most once per entity for the pass's lifetime
		// and for at most MaxAnimationSystemDiagnostics entities, the most the AnimationSystem keeps.
		void Run(Scene& scene, std::vector<AnimationSceneIssue>& findings);
	private:
		// One Skeleton's joint indices by name, kept while a pass uses it (the reference keeps its address unique).
		struct SkeletonJoints
		{
			AssetRef<SkeletonData> Skeleton{};
			std::map<std::string, uint32_t, std::less<>> Joints{}; // names found; a missing name is looked up each time
			bool Used = false;
		};

		[[nodiscard]] std::optional<uint32_t> FindJoint(const AssetRef<SkeletonData>& skeleton, std::string_view name);
	private:
		std::vector<SkeletonJoints> m_Skeletons{};
		std::set<UUID> m_Reported{};
	};

	// Decision R: the Animator entity whose rendered pose `entity` (readable) follows: the recorded Animator of the nearest
	// entity at or above `entity` holding a BoneAttachmentStateComponent whose Animator is still an ancestor of it; an invalid
	// handle when there is none, at once (one view emptiness check) in a scene without any, and in an edit scene.
	[[nodiscard]] ConstEntity FindAttachedRenderAnimator(ConstEntity entity);

	// The current pose of `entity` relative to `animator` (a strict ancestor, asserted): the product of the local matrices
	// (TransformSystem::ComputeLocalMatrix) of the entities below `animator` down to `entity`, root side first, which is
	// inverse(World(animator)) * World(entity) without an inverse.
	[[nodiscard]] glm::mat4 ComputeAttachedRelativeMatrix(ConstEntity animator, ConstEntity entity);

}
