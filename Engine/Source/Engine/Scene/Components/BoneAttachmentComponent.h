#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <string>

namespace Engine {

	// Registry name "BoneAttachment", version 1, category Animation (M41, Docs/Decisions/0058-m41-bone-attachments.md
	// decision 3; Architecture §5.3): places its entity on a joint of the Animator's Skeleton (a bone), the Animator being
	// the nearest one above the entity (on its parent or the parent's nearest ancestor that holds one, FindDrivingAnimator
	// started at the parent; never the entity's own). Requires Transform. Each fixed step, in the AnimationSystem phase's
	// attaching pass (Scene/BoneAttachments.h), after every Animator was posed and before the animation events, the entity's
	// Translation and Rotation are written so that, in the Animator entity's space, it sits at the joint's position plus
	// Offset turned by the joint's rotation, turned by the joint's rotation times Rotation; its Scale stays as authored and
	// the joint's scale is ignored. Its subtree renders at the Animator's rendered pose (Scene/RenderExtraction.h, "Attached
	// subtrees"), as the skin does. Nothing is placed in edit scenes or by the editor's animation preview. A missing Animator
	// or joint, an empty Bone, a Skeleton without a pose, and an entity whose own physics fights the placement are
	// ANIMATION_ATTACHMENT_INVALID.
	struct BoneAttachmentComponent
	{
		std::string Bone{};                                     // a joint name of the Animator's Skeleton, at most MaxSkeletonJointNameBytes of UTF-8
		glm::vec3 Offset = glm::vec3(0.0f);                     // metres, in the bone's axes
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // relative to the bone, unit
	};

}
