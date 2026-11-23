#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/ValidationContext.h"

#include <string>
#include <string_view>
#include <vector>

// The animation checks of one scene (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 30): what project.validate
// reports about a scene's SkinnedMeshes and Animators. The codes are appended after PREFAB_CYCLE in both of
// ProjectValidator.cpp's code tables in landing order (ADR 0028's phase-2 ledger: M23's after M21's and M22's at its merge),
// each with its Tests/Data/Validation/<CODE>/ fixture and its row in the game-debugging skill's validation-codes page; the
// AnimationSystem phase and the renderer behave as each code says, and the phase raises them once per entity at run time
// (AnimationSystem::GetDiagnostics).
//
// Frozen by the M23 contract C23.3; stream B implements it.

namespace Engine {

	class AssetManager;
	class Scene;

	// Warning, not fixable: a SkinnedMesh whose mesh has a skin has no Animator on its entity or an ancestor; it draws its
	// bind pose.
	inline constexpr std::string_view AnimationNoAnimatorCode = "ANIMATION_NO_ANIMATOR";
	// Warning, not fixable: a SkinnedMesh's mesh skin addresses more joints than the Skeleton of its Animator holds (a
	// skeleton that loads); it draws its bind pose.
	inline constexpr std::string_view AnimationSkeletonMismatchCode = "ANIMATION_SKELETON_MISMATCH";

	// One animation validation finding. M41 reuses it for ANIMATION_ATTACHMENT_INVALID (Scene/BoneAttachments.h), whose
	// findings name the BoneAttachment's entity and component.
	struct AnimationSceneIssue
	{
		std::string Code{}; // one of the codes above, or M41's AnimationAttachmentInvalidCode
		DiagnosticSeverity Severity = DiagnosticSeverity::Warning;
		UUID Entity{};           // the SkinnedMesh's (or the BoneAttachment's) entity
		std::string Component{}; // "SkinnedMesh" (or "BoneAttachment")
		std::string Field{};     // empty: the finding is about the component as a whole; "Bone" for a BoneAttachment's bone
		std::string Message{};
		std::string Hint{};

		bool operator==(const AnimationSceneIssue&) const = default;
	};

	// The animation checks over `scene` (effectively enabled entities with a SkinnedMesh and a MeshRenderer whose mesh loads
	// through `assets`, canonical order): ANIMATION_NO_ANIMATOR, then ANIMATION_SKELETON_MISMATCH against the Skeleton of the
	// entity's driving Animator (FindDrivingAnimator), when it loads. A mesh or skeleton that is missing or fails to load is
	// not checked (the asset checks report it). Pure function of its inputs otherwise; main thread.
	[[nodiscard]] std::vector<AnimationSceneIssue> FindAnimationSceneIssues(const Scene& scene, AssetManager& assets);

}
