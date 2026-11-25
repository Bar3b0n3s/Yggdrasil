#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>
#include <vector>

// The vehicles' debug view (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3 "The editor"; Architecture §8.13,
// §9.11): every wheel of every vehicle the vehicle composition lists, drawn with the colliders (RenderViewFlags::Colliders:
// the editor's collider view and viewport.screenshot's colliders annotation, through AppendRenderAnnotations after the
// joints), as debug lines of the Overlays declaration's two DebugLines pipelines: the wheel's circle at its pose with one
// spoke (which turns with the wheel's spin), its suspension's travel from its attachment point down along the body's -Y, and,
// in play, the ground's normal from its contact point, coloured by the vehicle's state. In an edit scene the records come from
// the components (each wheel hanging at its authored centre, at full droop, as the session would make it now); in play, from
// the session's vehicle (PhysicsSystem::GetVehicleInfo) and the wheel entities the session places every step, at the rendered
// pose, so a wheel stays on its mesh between steps as the colliders do. No pipeline, pass or golden of its own (ADR 0054
// decision 11's rule).

namespace Engine {

	class DebugDrawList;
	class PhysicsLayerTable;
	class PhysicsSystem;
	class Scene;

	// A vehicle's state as the view colours it.
	enum class VehicleDebugState : uint8_t
	{
		InForce, // pink: made (play), or one the session would make (edit)
		Refused  // rust: PHYSICS_VEHICLE_INVALID, a body it cannot drive, or (in play) any vehicle the session did not make
	};

	// One wheel's record, in world space.
	struct VehicleWheelDebugShape
	{
		UUID Vehicle{}; // the vehicle's entity
		UUID Wheel{};   // the wheel's entity
		VehicleDebugState State = VehicleDebugState::InForce;
		glm::vec3 Center = glm::vec3(0.0f);
		// The wheel's unit axle (its local +X) and its unit up (its local +Y, which turns with the spin in play): the circle lies in
		// the plane through Center normal to Axle, and the spoke runs along Up.
		glm::vec3 Axle = glm::vec3(1.0f, 0.0f, 0.0f);
		glm::vec3 Up = glm::vec3(0.0f, 1.0f, 0.0f);
		float Radius = 0.0f;
		// The suspension's attachment point and where the wheel's centre hangs at full droop (SuspensionMaxLength below it along
		// the body's -Y).
		glm::vec3 Attachment = glm::vec3(0.0f);
		glm::vec3 SuspensionEnd = glm::vec3(0.0f);
		// In play, while the wheel touches the ground: where, and the ground's unit normal.
		bool Touching = false;
		glm::vec3 ContactPoint = glm::vec3(0.0f);
		glm::vec3 ContactNormal = glm::vec3(0.0f);
	};

	// The number of lines of a wheel's circle, and the length of a contact's normal line in metres.
	inline constexpr uint32_t VehicleDebugCircleSegments = 16;
	inline constexpr float VehicleDebugNormalLength = 0.5f;

	// The colour of a vehicle of `state` (linear RGBA): none of ColliderDebugCategory's or JointDebugState's, so a wheel inside
	// its body's outline stands out, and mid-tone and saturated, so it reads over a light scene and a dark one.
	[[nodiscard]] glm::vec4 GetVehicleDebugColor(VehicleDebugState state);

	// The records of `scene`'s vehicles' wheels, vehicle by vehicle in canonical order and each vehicle's wheels in its wheel
	// order (ComposePhysicsVehicles over ComposePhysicsBodies(scene, layers)), at the rendered pose of interpolation alpha `alpha`
	// in [0, 1] (asserted) as BuildColliderDebugDraw's: with `physics` (the session's), a vehicle in force is drawn where the
	// session placed its wheel entities, with its suspension's length and each wheel's contact after the last step, and any other
	// as Refused at its components' places; without it (an edit scene), every vehicle at its components' places, Refused when
	// the composition refuses it. A wheel whose frame cannot be made is drawn at its entity's position. An empty list, composing
	// nothing, for a scene without a WheeledVehicle. Main thread.
	[[nodiscard]] std::vector<VehicleWheelDebugShape> BuildVehicleDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics,
		float alpha = 1.0f);

	// Appends `shapes` to `list` as lines (duration 0, DepthMode OnTop, so a wheel inside its mesh shows): the circle's
	// VehicleDebugCircleSegments lines, the spoke, the suspension's travel and, when Touching, the contact's normal.
	void AppendVehicleDebugDraw(std::span<const VehicleWheelDebugShape> shapes, DebugDrawList& list);

}
