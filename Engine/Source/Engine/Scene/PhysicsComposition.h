#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

// The physics composition rules (Architecture §5.3 "Physics composition rules", §9.2): which bodies a scene's components
// make, which colliders each body is built from, and what cannot be simulated as authored. One pure function, used by
// the play session's PhysicsSystem (which creates the bodies) and by the project validator and collider debug draw (which
// report and draw them), so the rules exist once.
//
// The rules, over effectively enabled entities only (HierarchyDisabledTag excluded; an edit scene, which has no such tag,
// is read through DisabledTag on the entity and its ancestors):
//   - An entity with a RigidBody owns a body (PhysicsBodyOrigin::RigidBody) of its Type. Its own colliders, and the solid
//     colliders of descendants that have no RigidBody or CharacterController of their own (and none between them and the
//     owner), form the body's shape: one collider alone, or a StaticCompoundShape (§5.3 "Solid colliders", "Shared static
//     bodies"). Its colliders must agree on IsTrigger (PHYSICS_MIXED_TRIGGER); when they are all triggers the body is a
//     sensor, which a Dynamic body cannot be (PHYSICS_DYNAMIC_TRIGGER). A sensor of Type Static is planned Kinematic:
//     triggers are Kinematic and kept active, so they also detect sleeping bodies (§9.2; Jolt's static sensors see only
//     active ones), and it follows its entity through MoveKinematic every PreStep like an implicit sensor body.
//     Descendants' trigger colliders never join it. A RigidBody left without any collider (a level root whose children
//     are all triggers, a collider removed at run time) is listed but makes no body: Jolt needs a shape, and the body
//     would collide with nothing.
//   - An entity with a CharacterController owns a character (PhysicsBodyOrigin::Character, §9.6) whose shape is its capsule.
//     Solid colliders on it and on its collider-only descendants form no body (the capsule is the character's shape).
//   - Solid colliders with no RigidBody on their entity or an ancestor (and no CharacterController above them) form an
//     implicit static body owned by their entity (PhysicsBodyOrigin::ImplicitStatic), one per entity: its own solid
//     colliders, plus those of collider-only descendants below it, exactly as a RigidBody would gather them.
//   - Trigger colliders on an entity without its own RigidBody form an implicit sensor body owned by that entity
//     (PhysicsBodyOrigin::ImplicitSensor): Kinematic, following the entity's world pose through MoveKinematic every
//     PreStep (§5.3 "Trigger colliders"). Only the entity's own trigger colliders belong to it.
//   - Implicit bodies are on the layer of the nearest ancestor RigidBody, else on layer 0 ("Default"): collider
//     components have no layer of their own (Docs/Decisions/0014-m11-decisions.md decision 10).
//   - Every body's collision group (PhysicsBodyPlan::CollisionGroup) is the UUID of the nearest entity at or above its
//     owner that has a RigidBody or a CharacterController: the owner itself for RigidBody and Character bodies, the body
//     or character a trigger is attached to for an implicit sensor body, none for implicit static bodies. Bodies of one
//     group never collide (BodyDescription::CollisionGroup), so a trigger attached to a moving ball or the pickup radius
//     of a character never reports its own body or character (§5.3 "a trigger attached to a moving ball ... just works").
//   - A body's colliders are ordered: the owner's own, in BuiltinComponents order (BoxCollider, SphereCollider,
//     CapsuleCollider, MeshCollider), then each gathered descendant in canonical order (§5.1), each in component order.
//     A collider's index in that order is its sub-shape user data (§9.2 "Collider identity in compounds").
//   - Bodies are listed in canonical order of their owners; an owner of both an implicit static and an implicit sensor body
//     lists the static one first. This is the order the session creates them in (§9.2 "Bodies are created in canonical
//     order"), so body handles are deterministic.
//
// Determinism: a pure function of the scene and its arguments; canonical order throughout. Main thread (the scene).
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 10).

namespace Engine {

	class AssetManager;
	class Scene;

	// Why a body exists.
	enum class PhysicsBodyOrigin : uint8_t
	{
		RigidBody,      // the owner's RigidBodyComponent
		ImplicitStatic, // solid colliders without a RigidBody above them (§5.3)
		ImplicitSensor, // trigger colliders on an entity without its own RigidBody (§5.3)
		Character       // the owner's CharacterControllerComponent (§9.6); its body is the controller's inner body
	};

	// The collider component a collider comes from.
	enum class PhysicsColliderType : uint8_t
	{
		Box,
		Sphere,
		Capsule,
		Mesh
	};

	// One collider of a body.
	struct PhysicsColliderPlan
	{
		UUID Entity{}; // the entity that holds the collider component (the owner, or a gathered descendant)
		PhysicsColliderType Type = PhysicsColliderType::Box;
		bool IsTrigger = false;
	};

	// One body the scene makes.
	struct PhysicsBodyPlan
	{
		UUID Owner{}; // the entity whose world pose the body has (and, for Dynamic bodies, writes back, §9.3)
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		// RigidBody.Type for RigidBody bodies, except Kinematic for a sensor RigidBody of Type Static; Static for implicit
		// static bodies; Kinematic for implicit sensor bodies and characters' inner bodies.
		PhysicsMotionType MotionType = PhysicsMotionType::Static;
		bool IsSensor = false;
		// The resolved project layer index (an unknown name resolves to 0 with PHYSICS_UNKNOWN_LAYER).
		uint32_t Layer = 0;
		// The nearest entity at or above the owner with a RigidBody or a CharacterController (see the rules above); the
		// invalid UUID for none. The session passes its value as BodyDescription::CollisionGroup (and
		// CharacterControllerDescription::CollisionGroup).
		UUID CollisionGroup{};
		// The colliders in sub-shape user data order (index = user data); empty for characters.
		std::vector<PhysicsColliderPlan> Colliders{};
		// False when an Error diagnostic of the composition refuses the body (PHYSICS_MIXED_TRIGGER,
		// PHYSICS_DYNAMIC_TRIGGER, PHYSICS_NONCONVEX_DYNAMIC, PHYSICS_ALL_DOFS_LOCKED, or PHYSICS_LIMIT_EXCEEDED for the
		// bodies beyond the limit), and, without a diagnostic, for a RigidBody without colliders: it is listed, so
		// validators and debug draw see it, but never created (and never counts against the limit).
		bool IsCreatable = true;
	};

	struct PhysicsComposition
	{
		std::vector<PhysicsBodyPlan> Bodies{};
		// The composition's diagnostics (every code but PHYSICS_INVALID_SHAPE, which needs the shapes built, and
		// PHYSICS_ADJACENT_STATIC_BODIES, which needs their bounds: PhysicsValidation.h), sorted by (entity UUID, code,
		// subject).
		std::vector<PhysicsDiagnostic> Diagnostics{};
	};

	// The bodies of `scene` under the rules above, with layer names resolved against `layers` and at most `maxBodies` bodies
	// creatable (characters included; the rest are listed with IsCreatable false and one PHYSICS_LIMIT_EXCEEDED Error, Subject
	// "bodies", on each refused owner). Reports PHYSICS_MIXED_TRIGGER, PHYSICS_DYNAMIC_TRIGGER, PHYSICS_NONCONVEX_DYNAMIC,
	// PHYSICS_ALL_DOFS_LOCKED, PHYSICS_UNKNOWN_LAYER, PHYSICS_NONUNIFORM_SCALE (a sphere or capsule collider whose entity's
	// world scale is not uniform; the world scale is TransformSystem::GetWorldScale), PHYSICS_DYNAMIC_UNDER_MOVING_PARENT
	// (a Dynamic RigidBody with an ancestor that owns a Kinematic or Dynamic RigidBody or a CharacterController) and
	// PHYSICS_LIMIT_EXCEEDED. Works on edit and runtime scenes; reads world poses by walking the parent chain, so it needs no
	// TransformSystem::Update first.
	[[nodiscard]] PhysicsComposition ComposePhysicsBodies(const Scene& scene, const PhysicsLayerTable& layers,
		uint32_t maxBodies = PhysicsWorldLimits::DefaultMaxBodies);

	// The shapes of mesh colliders, built once per (mesh handle, asset version, convex, scale) and shared (§9.2: "Mesh
	// shapes are cached per (mesh handle, version, scale)"): each entry is a PhysicsShape of one collider (the mesh's
	// vertex positions as a ConvexHullShapeGeometry when convex, its triangles as a MeshShapeGeometry otherwise) at that
	// scale, which bodies place through SharedShapeGeometry, so rebuilding a compound never rebuilds its meshes' BVHs and a
	// hot reload (a new version, §7.5) builds new ones. The play session's PhysicsSystem owns one; the validator and the
	// collider visualization use none. Main thread; not copyable.
	class PhysicsMeshShapeCache
	{
	public:
		PhysicsMeshShapeCache();
		~PhysicsMeshShapeCache();

		PhysicsMeshShapeCache(const PhysicsMeshShapeCache&) = delete;
		PhysicsMeshShapeCache& operator=(const PhysicsMeshShapeCache&) = delete;

		// The shape of `mesh` at its current AssetManager::GetVersion in `assets` (loaded with GetOrPlaceholder, so a
		// missing mesh records ASSET_MISSING and gives the placeholder cube's shape), as a convex hull when `convex` and a
		// triangle mesh otherwise, at `scale` (compared bit for bit), built with PhysicsShape::Create on first use. Errors:
		// PhysicsShape::Create's (Validation "PHYSICS_INVALID_SHAPE: ..."), which are not cached.
		[[nodiscard]] Result<Ref<const PhysicsShape>> GetOrCreate(AssetManager& assets, AssetHandle mesh, bool convex, const glm::vec3& scale);
		// Drops every entry that no body holds any more (its shape referenced by the cache alone): the system calls it after
		// the rebuilds of a PreStep, so replaced versions and scales do not accumulate.
		void Prune();
		void Clear();
		// The number of entries.
		[[nodiscard]] size_t GetSize() const;
	private:
		// The entries by key (Scene/PhysicsComposition.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// The shape description of `plan` (a plan ComposePhysicsBodies returned for `scene`, not a character): each collider's
	// geometry from its component (BoxCollider.HalfExtents, SphereCollider.Radius, CapsuleCollider.Radius and HalfHeight;
	// a MeshCollider's mesh, or the entity's MeshRenderer mesh when its Mesh is null, as a ConvexHullShapeGeometry of the
	// vertex positions when Convex and a MeshShapeGeometry otherwise), placed relative to the owner: the collider's
	// Offset and Rotation in its entity's space, then that entity's transform relative to the owner, with every world scale
	// on the way baked in (the scale is taken along the collider's axes, which is exact for uniform scales and for
	// rotations that map axes onto axes). Spheres and capsules under a non-uniform scale use its largest component
	// (PHYSICS_NONUNIFORM_SCALE is ComposePhysicsBodies'). UserData is the collider's index in plan.Colliders. Meshes load
	// through `assets` (AssetManager::GetOrPlaceholder, so a missing mesh records ASSET_MISSING and uses the placeholder
	// cube); null `assets` makes every mesh collider an error. With `meshShapes` (the session's cache), a mesh collider is
	// instead a SharedShapeGeometry of meshShapes->GetOrCreate(*assets, mesh, Convex, its baked scale) with a Scale of 1,
	// so no vertex is copied. Errors: Validation "PHYSICS_INVALID_SHAPE: ..." naming the collider entity for a mesh
	// collider without a mesh (no Mesh and no MeshRenderer mesh, or no asset manager), for a relative transform that cannot
	// be decomposed (TransformSystem::DecomposeMatrix fails) and for a cached mesh shape Jolt refuses. Other shapes Jolt
	// refuses are found by PhysicsShape::Create on the result.
	[[nodiscard]] Result<BodyShapeDescription> DescribePhysicsBodyShape(const Scene& scene, const PhysicsBodyPlan& plan, AssetManager* assets,
		PhysicsMeshShapeCache* meshShapes = nullptr);

	// "RigidBody", "ImplicitStatic", "ImplicitSensor" or "Character".
	[[nodiscard]] std::string_view PhysicsBodyOriginToString(PhysicsBodyOrigin origin);

}
