#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

// The 2D composition rules (Docs/Decisions/0032-m21-contract.md decision 9): PhysicsComposition.h's rules in the plane, with
// RigidBody2D in RigidBody's place and BoxCollider2D, CircleCollider2D, PolygonCollider2D and (M21, C21.2) TilemapCollider in
// the collider components'.
// One pure function, used by the play session's Physics2DSystem (which creates the bodies) and by ValidateScenePhysics2D
// (which reports them), so edit-time and run-time diagnostics agree, as for the 3D world. Private to Scene.
//
// The rules, over effectively enabled entities (DisabledTag on the entity or an ancestor excludes its subtree):
//   - One world per entity. An entity that holds a 2D physics component (RigidBody2D or a 2D collider) and a 3D one
//     (RigidBody, BoxCollider, SphereCollider, CapsuleCollider, MeshCollider, CharacterController) is PHYSICS2D_MIXED_WORLDS
//     (Error) and counts as holding no 2D component: it neither owns, carries nor gives a collider to a 2D body. The 3D
//     composition (PhysicsComposition.h) never looks at 2D components, so its bodies are those of the scene without them.
//   - The rest are PhysicsComposition.h's: a RigidBody2D owns a body (PhysicsBodyOrigin::RigidBody) of its Type, gathering
//     the solid 2D colliders of descendants without a RigidBody2D of their own (none between); its colliders agree on
//     IsTrigger (PHYSICS_MIXED_TRIGGER), a sensor is not Dynamic (PHYSICS_DYNAMIC_TRIGGER), a sensor of Type Static is
//     planned Kinematic; solid 2D colliders without a RigidBody2D above form an implicit static body owned by the topmost
//     such entity; trigger 2D colliders on an entity without its own RigidBody2D form its implicit sensor body; implicit
//     bodies take the nearest ancestor RigidBody2D's layer, else layer 0; the collision group is the nearest entity at or
//     above the owner with a RigidBody2D; a body's colliders are the owner's own (BoxCollider2D, CircleCollider2D,
//     PolygonCollider2D, TilemapCollider order), then each gathered descendant's in canonical order; bodies are listed in
//     canonical order of their owners, an implicit static body before an implicit sensor body; PHYSICS_UNKNOWN_LAYER,
//     PHYSICS_DYNAMIC_UNDER_MOVING_PARENT (an ancestor owns a Kinematic or Dynamic RigidBody2D), PHYSICS_NONUNIFORM_SCALE
//     (a CircleCollider2D whose entity's world scale differs in X and Y; Z does not count) and PHYSICS_LIMIT_EXCEEDED.
//   - The plane. PHYSICS2D_OFF_PLANE (Warning) on every creatable body's owner, and every entity whose colliders it
//     gathers, whose world rotation is not a rotation about Z alone: its swing (the rotation left once its twist about Z is
//     taken out) turns by more than IsOffPlaneRotation's tolerance. The body takes the twist, and the gathered colliders
//     their placement's twist relative to the owner (DescribePhysics2DBodyShape).
//   - Tilemaps (Docs/Decisions/0032-m21-contract.md decision 25). A TilemapCollider is one collider of its plan, like the
//     other three, whatever its tilemap holds: the composition never loads an asset. Its shape is its tilemap's merged
//     boxes (Asset/TilemapCollision.h), which DescribePhysics2DBodyShape loads through an asset manager: every box is a
//     sub-shape with the collider's user data, and a tilemap without a solid cell adds none, so a body whose colliders add
//     no sub-shape at all has an empty description and is not created, without a diagnostic (as a RigidBody2D without
//     colliders).
//
// Determinism: a pure function of the scene and its arguments (and, for the shapes, of the loaded tilemaps); canonical order
// throughout; DetMath only. Main thread.

namespace Engine {

	class AssetManager;
	class ConstEntity;
	class Scene;

	// The 2D collider component a collider comes from.
	enum class Physics2DColliderType : uint8_t
	{
		Box,
		Circle,
		Polygon,
		Tilemap // M21 (C21.2): a TilemapCollider, its tilemap's merged boxes
	};

	struct Physics2DColliderPlan
	{
		UUID Entity{};
		Physics2DColliderType Type = Physics2DColliderType::Box;
		bool IsTrigger = false;
	};

	// One 2D body the scene makes; its members mean PhysicsBodyPlan's (Origin is never Character).
	struct Physics2DBodyPlan
	{
		UUID Owner{};
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		PhysicsMotionType MotionType = PhysicsMotionType::Static;
		bool IsSensor = false;
		uint32_t Layer = 0;
		UUID CollisionGroup{};
		std::vector<Physics2DColliderPlan> Colliders{};
		bool IsCreatable = true;
	};

	struct Physics2DComposition
	{
		std::vector<Physics2DBodyPlan> Bodies{};
		// Sorted by (entity UUID, code, subject).
		std::vector<PhysicsDiagnostic> Diagnostics{};
	};

	// Whether `entity` holds a 2D physics component, and whether it holds a 3D one.
	[[nodiscard]] bool HasPhysics2DComponent(ConstEntity entity);
	[[nodiscard]] bool HasPhysics3DComponent(ConstEntity entity);

	// Whether a unit rotation is not a rotation about Z alone: the sine of half its swing angle, sqrt(x^2 + y^2), exceeds
	// PhysicsUnitRotationTolerance (a swing of about 0.11 degrees), so the rounding of a rotation read back from a matrix never
	// counts.
	[[nodiscard]] bool IsOffPlaneRotation(const glm::quat& rotation);

	// The 2D bodies of `scene` under the rules above (layer names resolved against `layers`, at most `maxBodies` creatable).
	// Works on edit and runtime scenes; walks the parent chain, so it needs no TransformSystem::Update first.
	[[nodiscard]] Physics2DComposition ComposePhysics2DBodies(const Scene& scene, const PhysicsLayerTable& layers,
		uint32_t maxBodies = PhysicsWorldLimits::DefaultMaxBodies);

	// The tilemap a TilemapCollider on `entity` collides with (decision 25): its Tilemap, else the entity's
	// TilemapRenderer.Tilemap; the null handle for neither, and for an entity without a TilemapCollider.
	[[nodiscard]] AssetHandle GetTilemapColliderTilemap(ConstEntity entity);

	// The shape description of `plan` (a plan of ComposePhysics2DBodies for `scene`), relative to its owner: each collider's
	// geometry from its component (BoxCollider2D.HalfExtents, CircleCollider2D.Radius, PolygonCollider2D.Points), placed by
	// its Offset and Angle in its entity's local XY plane, then by that entity's transform relative to the owner, with every
	// world scale on the way baked in (as DescribePhysicsBodyShape); the placement's position in the plane, its twist about Z
	// and its scale along the collider's X and Y axes give the collider's Position, Angle and Scale. A circle under a
	// non-uniform scale uses the larger of X and Y (PHYSICS_NONUNIFORM_SCALE is the composition's). UserData is the collider's
	// index in plan.Colliders.
	//
	// A TilemapCollider adds one BoxShape2D per box of MergeTilemapBoxes, in that order (its chunks in canonical order), each
	// with the collider's UserData: half the box's cells' rectangle (TilemapData.h: cell (x, y) spans [x CellSize.x,
	// (x + 1) CellSize.x] x [y CellSize.y, (y + 1) CellSize.y]) as its half extents, centred on the rectangle's centre in
	// its entity's local XY plane (a TilemapCollider has no offset or angle), placed and scaled as the other colliders. Its
	// tilemap (GetTilemapColliderTilemap) loads through `assets` (AssetManager::Load). A tilemap without a solid cell adds no
	// box, so the description may hold no collider at all: the body is then not created, without a diagnostic.
	//
	// Errors: Validation "PHYSICS_INVALID_SHAPE: ..." located at the collider entity: for a placement that cannot be
	// decomposed; and, as "PHYSICS_INVALID_SHAPE: collider <i>: ..." with i the collider's index in plan.Colliders, for a
	// TilemapCollider without a tilemap (neither its Tilemap nor its entity's TilemapRenderer's), with null `assets`, whose
	// tilemap is missing, fails to load or is not a Tilemap, or whose tilemap merges into more than MaxTilemapColliderBoxes
	// boxes. CreatePhysics2DBodyShape finds the rest.
	[[nodiscard]] Result<Body2DShapeDescription> DescribePhysics2DBodyShape(const Scene& scene, const Physics2DBodyPlan& plan, AssetManager* assets);

	// The shape of a body: MakeBody2DShape, then PhysicsShape::Create, of `description` (DescribePhysics2DBodyShape's for
	// `plan`, at least one collider). A refusal that names a collider by its place in description.Colliders ("collider <i>:
	// ...") names it instead by its index in plan.Colliders, its UserData (a TilemapCollider's boxes are several places of
	// one collider, so it also names the box), located at the collider's entity. Errors: MakeBody2DShape's and
	// PhysicsShape::Create's (Validation "PHYSICS_INVALID_SHAPE: ...").
	[[nodiscard]] Result<Ref<const PhysicsShape>> CreatePhysics2DBodyShape(const Physics2DBodyPlan& plan, const Body2DShapeDescription& description);

	// The collider of `plan` that `error`, a PHYSICS_INVALID_SHAPE refusal of DescribePhysics2DBodyShape or
	// CreatePhysics2DBodyShape, is about: the one its message names ("<code>: collider <i>: ..."), else the first collider of
	// the entity it is located at; nullptr for neither.
	[[nodiscard]] const Physics2DColliderPlan* FindRefusedCollider2D(const Physics2DBodyPlan& plan, const Error& error);

	// The field the PHYSICS_INVALID_SHAPE of a refused collider names: "Tilemap" for a TilemapCollider (decision 25: its
	// tilemap is what is wrong, or what makes the boxes), else none.
	[[nodiscard]] std::string_view GetCollider2DShapeField(Physics2DColliderType type);

	// "BoxCollider2D", "CircleCollider2D", "PolygonCollider2D" or "TilemapCollider".
	[[nodiscard]] std::string_view GetCollider2DComponentName(Physics2DColliderType type);

}
