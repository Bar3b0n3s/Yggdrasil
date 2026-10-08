#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsLayers.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <vector>

// Collider visualization (Architecture §8.10 "Filled by ... collider visualization (built from components, so it works in
// edit mode)", Roadmap M11 "collider debug draw (edit and play)"): the wire shapes of every collider, coloured by the kind
// of body it belongs to, built from the components and the composition rules (PhysicsComposition.h), so an edit scene
// shows exactly the bodies a play session will create, and a play session adds the sleeping state of its bodies.
//
// The shapes are renderer-agnostic records in the vocabulary of the renderer's DebugDrawList (§8.10: Box, Sphere, Capsule,
// Line), so the render snapshot's debug draw list (M8) takes them one to one: Box, Sphere and Capsule records become the
// list's Box, Sphere and Capsule entries, Lines records one Line per vertex pair, each with the record's colour, duration 0
// (this frame) and DepthMode Tested. Render extraction appends them when the view asks for colliders (the editor's
// viewport option and the screenshot annotation "colliders", M9/M10); the adapter and that request flag land with the
// merge of M8's DebugDrawList (Docs/Decisions/0014-m11-decisions.md decision 16), so the tests of this unit check the
// records, not pixels.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 16).

namespace Engine {

	class AssetManager;
	class PhysicsSystem;
	class Scene;

	// What a collider's body is, which picks its colour.
	enum class ColliderDebugCategory : uint8_t
	{
		Static,    // a Static RigidBody or an implicit static body
		Kinematic, // a Kinematic RigidBody (triggers excluded)
		Dynamic,   // an awake Dynamic RigidBody (every Dynamic body in an edit scene)
		Sleeping,  // a sleeping Dynamic RigidBody (play sessions only)
		Trigger,   // a sensor: trigger colliders, implicit sensor bodies
		Character, // a CharacterController's capsule
		// A body that is not created: refused by the composition (PhysicsBodyPlan::IsCreatable false), by its shape (an edit
		// scene builds each body's shape as the session does) or by the session (play).
		Invalid
	};

	// The colour of a category (linear RGBA).
	[[nodiscard]] constexpr glm::vec4 GetColliderDebugColor(ColliderDebugCategory category)
	{
		switch (category)
		{
			case ColliderDebugCategory::Static:    return glm::vec4(0.55f, 0.55f, 0.6f, 1.0f);
			case ColliderDebugCategory::Kinematic: return glm::vec4(0.3f, 0.6f, 1.0f, 1.0f);
			case ColliderDebugCategory::Dynamic:   return glm::vec4(0.3f, 1.0f, 0.4f, 1.0f);
			case ColliderDebugCategory::Sleeping:  return glm::vec4(0.15f, 0.45f, 0.2f, 1.0f);
			case ColliderDebugCategory::Trigger:   return glm::vec4(1.0f, 0.8f, 0.2f, 1.0f);
			case ColliderDebugCategory::Character: return glm::vec4(0.8f, 0.4f, 1.0f, 1.0f);
			case ColliderDebugCategory::Invalid:   return glm::vec4(1.0f, 0.2f, 0.2f, 1.0f);
		}
		return glm::vec4(1.0f);
	}

	// The primitive a record draws.
	enum class ColliderDebugShapeType : uint8_t
	{
		Box,
		Sphere,
		Capsule,
		Lines
	};

	// One collider's wire shape, in world space.
	struct ColliderDebugShape
	{
		ColliderDebugShapeType Type = ColliderDebugShapeType::Box;
		ColliderDebugCategory Category = ColliderDebugCategory::Static;
		glm::vec4 Color = glm::vec4(1.0f); // GetColliderDebugColor(Category)
		// The shape's frame: the collider's centre and orientation in world space (its entity's world pose with the
		// collider's Offset and Rotation applied; no scale). Unused by Lines.
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		// World dimensions, the entity's world scale applied as the body's shape applies it (DescribePhysicsBodyShape):
		// Box: HalfExtents; Sphere: Radius; Capsule: Radius and HalfHeight (the cylinder's, along the frame's Y).
		glm::vec3 HalfExtents = glm::vec3(0.5f);
		float Radius = 0.5f;
		float HalfHeight = 0.5f;
		// Lines: world-space segment end points, two per segment (a mesh collider's triangle edges, each edge once).
		std::vector<glm::vec3> LineVertices{};
		// The collider entity (the CharacterController's entity for a capsule).
		UUID Collider{};
		// The owner of the body the collider belongs to.
		UUID Body{};

		bool operator==(const ColliderDebugShape&) const = default;
	};

	struct ColliderDebugDrawOptions
	{
		// Include trigger colliders (and implicit sensor bodies).
		bool Triggers = true;
		// Include CharacterControllers' capsules.
		bool Characters = true;
		// A mesh collider with more distinct triangle edges than this draws its local bounding box (a Box record) instead.
		uint32_t MaxMeshEdges = 65536;
	};

	// The wire shapes of `scene`'s colliders: one record per collider of every body ComposePhysicsBodies(scene, layers)
	// lists, in its order (bodies in canonical order of their owners, colliders in sub-shape order), plus one Capsule per
	// character. Mesh colliders draw their mesh's triangle edges (the source mesh's, also for Convex ones), loaded through
	// `assets` as DescribePhysicsBodyShape loads them; a mesh collider without a mesh (or with null `assets`) draws nothing.
	// With `physics` (the session's PhysicsSystem; null for an edit scene), awake and sleeping Dynamic bodies are told
	// apart, and a body the session did not create is Invalid; without it, each body's shape is built as the session builds
	// it (DescribePhysicsBodyShape, PhysicsShape::Create, PhysicsShape::CheckDynamicBody; needs the PhysicsEngine) and a body
	// it refuses is Invalid. Poses are the entities' world poses (walking the parent chain), which equal the bodies' after
	// PostStep. Main thread; a pure function of its inputs.
	[[nodiscard]] std::vector<ColliderDebugShape> BuildColliderDebugDraw(const Scene& scene, const PhysicsLayerTable& layers, const PhysicsSystem* physics,
		AssetManager* assets, const ColliderDebugDrawOptions& options = {});

}
