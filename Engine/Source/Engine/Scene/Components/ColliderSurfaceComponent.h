#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "ColliderSurface", version 1, category Physics (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision
	// 1; Architecture §5.3, §9.2 "Collider surfaces"): the surface of every solid collider component on its own entity
	// (BoxCollider, SphereCollider, CapsuleCollider, MeshCollider and TerrainCollider that are not triggers), whichever body
	// gathers it (its own RigidBody's, an ancestor's compound, an implicit static body). Contacts with those colliders use
	// Friction and Restitution instead of their body's, combined with the other side's as bodies are (the geometric mean of
	// the frictions, the larger restitution), and SurfaceVelocity, the velocity the surface moves at while the body stays
	// where it is (a conveyor belt): a body resting on it is carried by friction, and a character standing on it is carried
	// whatever its InheritGroundVelocity says, each by the velocity's part along the surface alone (a character's projected
	// onto the plane of its ground normal), so a part off the surface lifts and presses nothing. Requires Transform. Not descendants' colliders, triggers, a character's capsule
	// or 2D colliders (PHYSICS_SURFACE_UNUSED). Friction in [0, MaxPhysicsFriction] and Restitution in [0, 1], as RigidBody's,
	// whose defaults these are, so a component added for its velocity alone changes nothing else. SurfaceVelocity in m/s in
	// the entity's own axes (its world rotation, never its scale), each component within MaxCharacterSpeed. A write takes
	// effect at the next physics pre-step without recomposing the scene.
	struct ColliderSurfaceComponent
	{
		float Friction = 0.5f;
		float Restitution = 0.0f;
		glm::vec3 SurfaceVelocity = glm::vec3(0.0f);
	};

}
