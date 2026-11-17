#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsJoint.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// The joints' debug view (M37, Docs/Decisions/0054-m37-joints.md decision 11; Architecture §8.13, §9.10): every joint the
// joint composition lists, drawn with the colliders (RenderViewFlags::Colliders: the editor's collider view and
// viewport.screenshot's colliders annotation, through AppendRenderAnnotations after the colliders), as debug lines of the
// Overlays declaration's two DebugLines pipelines: a small cross at the joint's point on its body, a hinge's or slider's
// axis through it, and a line from that point to its point on the connected body (a distance joint's other end; the two
// points of a joint in force coincide but where the solver has not closed a gap), coloured by its state. In an edit scene
// the records are built from the components (as the session would make the joint now); in play, from the session's joints
// (PhysicsSystem::GetJointInfo: where the bodies hold them), each point at its body's rendered pose, so a joint stays on its
// meshes between steps as the colliders do.

namespace Engine {

	class DebugDrawList;
	class PhysicsLayerTable;
	class PhysicsSystem;
	class Scene;

	// A joint's state as the view colours it.
	enum class JointDebugState : uint8_t
	{
		InForce, // teal: made (play), or one the session would make (edit)
		Broken,  // orange: Broken
		Refused  // crimson: PHYSICS_JOINT_INVALID, an end without a body, or a frame the world would refuse
	};

	// One joint's record, in world space.
	struct JointDebugShape
	{
		UUID Entity{};
		PhysicsJointType Type = PhysicsJointType::Hinge;
		JointDebugState State = JointDebugState::InForce;
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 ConnectedPoint = glm::vec3(0.0f);
		// A hinge's or slider's unit axis; zero for distance and fixed joints.
		glm::vec3 Axis = glm::vec3(0.0f);
	};

	// The size of a joint's cross and the length of its axis line, in metres.
	inline constexpr float JointDebugCrossSize = 0.1f;
	inline constexpr float JointDebugAxisLength = 0.5f;

	// The colour of a joint of `state` (linear RGBA): none of ColliderDebugCategory's, so a joint on its body's outline stands
	// out from the collider lines, and mid-tone and saturated, so it reads over a light scene and a dark one.
	[[nodiscard]] glm::vec4 GetJointDebugColor(JointDebugState state);

	// The records of `scene`'s joints, in canonical order of their entities (ComposePhysicsJoints over ComposePhysicsBodies(scene,
	// layers)), at the rendered pose of interpolation alpha `alpha` in [0, 1] (asserted) as BuildColliderDebugDraw's: with
	// `physics` (the session's), a joint in force is drawn where the bodies hold it and any other at its components' places;
	// without it (an edit scene), every joint at its components' places, Refused when the session would refuse it (the
	// composition's reasons, a frame its entity's transform makes vanish, a point beyond MaxPhysicsCoordinate). A joint
	// whose frame cannot be made is drawn at its entity's position. An empty list, composing nothing, for a scene without a
	// joint component. Main thread.
	[[nodiscard]] std::vector<JointDebugShape> BuildJointDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics,
		float alpha = 1.0f);

	// Appends `shapes` to `list` as lines (duration 0, DepthMode OnTop, so a joint inside its body's mesh shows): three for the
	// cross, one for the axis when there is one, one to the connected point when it is apart from the point.
	void AppendJointDebugDraw(std::span<const JointDebugShape> shapes, DebugDrawList& list);

}
