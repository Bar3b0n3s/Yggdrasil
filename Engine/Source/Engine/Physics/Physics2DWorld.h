#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/ContactBuffer.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsQueries.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Physics/PhysicsWorld.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

// The 2D physics world (M21, C21.1, R-2D-PHYS; ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decision 8): a
// second Jolt world, one per play session beside the 3D one, owned by Scene/Physics2DSystem. It is a PhysicsWorld whose
// Dynamic bodies allow only translation along world X and Y and rotation about world Z (Physics2DDofs, PhysicsDofs maps bit
// for bit to Jolt's EAllowedDOFs), and whose shapes are the existing shape variant laid in the XY plane (MakeBody2DShape: a
// circle is a sphere, boxes and polygons are extruded along Z by GetPhysics2DHalfDepth on each side), so the frozen M11
// PhysicsShape.h does not change and no library is added. Its API speaks 2D (Physics2DPose: a position in the plane and an
// angle about +Z; velocities as glm::vec2 and a float) and keeps every body on the plane z = 0: poses, targets, forces,
// impulses, velocities and queries are given in the plane, and nothing it is handed can move a body off it.
//
// Plane locks: Jolt zeroes a Dynamic body's velocity along its locked degrees of freedom every step, so its Z position never
// changes and its rotation stays about Z (Jolt integrates an angular velocity about Z only); Kinematic bodies move only to
// planar targets (MoveKinematic) and Static ones never move. The extruded shapes cannot push a body out of the plane, since
// those directions are locked: "Physics2DWorld: a dynamic body never leaves its plane" proves it on a pile. Nor can a
// contact's normal leave the plane: each collider's depth grows with its size (GetPhysics2DHalfDepth), so the overlap of
// two shapes along Z always exceeds their deepest overlap in the plane, and Jolt's minimum-penetration axis, the contact
// normal, lies in the plane however deep they overlap ("Physics2DWorld: a body sunk deep into a box is pushed out in the
// plane").
//
// Everything else is PhysicsWorld's (Physics/PhysicsWorld.h): nothing invalid reaches Jolt (every call validates, a void
// mutator asserts and ignores), content errors carry PHYSICS_* codes, bodies get Jolt's IDs in creation order, contacts are
// drained unsorted from the world's ContactBuffer with M20's touching rule, the step is deterministic in every
// configuration and for every worker thread count, and it is main-thread only. Not copyable or movable.
//
// Frozen by the M21 contract (C21.1); stream P1 implements it.

namespace Engine {

	// The degrees of freedom of a Dynamic 2D body: translation along X and Y, rotation about Z.
	inline constexpr PhysicsDofs Physics2DDofs = PhysicsDofs::TranslationX | PhysicsDofs::TranslationY | PhysicsDofs::RotationZ;
	// A Dynamic 2D body with RigidBody2D.FixedRotation: translation along X and Y only.
	inline constexpr PhysicsDofs Physics2DTranslationDofs = PhysicsDofs::TranslationX | PhysicsDofs::TranslationY;

	// A pose in the plane: the body origin's position (z = 0) and its rotation about +Z in radians, counter-clockwise seen
	// from +Z.
	struct Physics2DPose
	{
		glm::vec2 Position = glm::vec2(0.0f);
		float Angle = 0.0f;

		bool operator==(const Physics2DPose&) const = default;
	};

	// The 3D pose of `pose`: Position (x, y, 0) and the rotation of Angle about +Z (Core/DetMath, so it is identical in every
	// configuration).
	[[nodiscard]] PhysicsPose ToPhysicsPose(const Physics2DPose& pose);
	// The planar part of `pose`: its X and Y, and the angle about +Z of its rotation's twist about Z (the rotation's swing
	// about X and Y dropped; DetMath::Atan2), in (-pi, pi].
	[[nodiscard]] Physics2DPose ToPhysics2DPose(const PhysicsPose& pose);

	// A rectangle centred on its origin. Both half extents > 0.
	struct BoxShape2D
	{
		glm::vec2 HalfExtents = glm::vec2(0.5f);
	};

	// A circle centred on its origin. Radius > 0.
	struct CircleShape2D
	{
		float Radius = 0.5f;
	};

	// The convex hull of Points (3 or more points spanning an area), in the shape's own space. Laid as a ConvexHullShapeGeometry,
	// whose corners Jolt rounds by its default convex radius (up to 0.05 m, less on a hull too small for it) while a box's
	// stay sharp (PhysicsShape.cpp): contacts meet the rounded hull and rays the sharp one (an accepted limit, Backlog §6).
	struct PolygonShape2D
	{
		std::vector<glm::vec2> Points{};
	};

	using Shape2DGeometry = std::variant<BoxShape2D, CircleShape2D, PolygonShape2D>;
	// The shapes an overlap query takes: those of Physics2D.OverlapBox and OverlapCircle and physics2d.overlap, laid as a box
	// and a sphere, which PhysicsWorld::Overlap accepts.
	using Overlap2DShape = std::variant<BoxShape2D, CircleShape2D>;

	// How far along Z, in metres, the 3D shape of a collider of `geometry` under `scale` extends on each side of the plane
	// z = 0 (it spans z in [-h, h]). A box or a polygon is extruded by the diagonal of its bounding rectangle after the scale
	// (2 |scale * HalfExtents| for a box; |max - min| over the scaled Points for a polygon), which is twice the radius of a
	// circle enclosing it; a circle, laid as a sphere, is not extruded and gives its scaled radius (Radius * |scale.x|).
	//   - Contact normals stay in the plane. Every 2D body lies on z = 0, so two shapes overlap along Z by the sum of their
	//     half depths, while their overlap in the plane never exceeds the sum of their enclosing radii (and a circle's own
	//     radius counts on both sides). So the overlap along Z always exceeds the deepest overlap in the plane, and Jolt's
	//     minimum-penetration axis, which becomes the contact normal, lies in the plane however deep two shapes overlap; a
	//     normal along the locked Z would push nothing, leaving a body teleported into a wall stuck in it or sinking.
	//   - The inertia is the 2D one. A box's or hull's inertia about Z does not depend on its depth (m (w^2 + h^2) / 12 for a
	//     box of mass m), and a depth proportional to the in-plane size keeps the inertia tensor's conditioning the same at
	//     every size, so the mass properties' isotropic floor (Physics/Private/PhysicsMassProperties.cpp) never adds to a
	//     small body's inertia about Z. A circle's inertia is its sphere's, 0.4 m r^2 (a disc's would be 0.5 m r^2).
	//   - The depth, 2h, is a collider dimension: PhysicsShape::Create refuses a box or polygon whose bounding rectangle's
	//     diagonal exceeds PhysicsShape::MaxColliderSize / 2 (50 km).
	// Pure; `geometry` and `scale` as MakeBody2DShape accepts them (asserted).
	[[nodiscard]] float GetPhysics2DHalfDepth(const Shape2DGeometry& geometry, const glm::vec2& scale);

	// One collider within its 2D body: Geometry scaled by Scale (a circle needs |Scale.x| = |Scale.y|), then rotated by Angle
	// (radians about +Z) and moved to Position, both relative to the body's origin in the plane. UserData is the collider
	// index contacts and queries report (ColliderShapeDescription::UserData).
	struct Collider2DShapeDescription
	{
		Shape2DGeometry Geometry = BoxShape2D{};
		glm::vec2 Scale = glm::vec2(1.0f);
		glm::vec2 Position = glm::vec2(0.0f);
		float Angle = 0.0f;
		uint32_t UserData = 0;
	};

	// A 2D body's whole shape: one collider, or several forming a compound. At least one.
	struct Body2DShapeDescription
	{
		std::vector<Collider2DShapeDescription> Colliders{};
	};

	// `description` laid in the XY plane as the existing 3D shape variant, each collider with its own half depth h
	// (GetPhysics2DHalfDepth of its Geometry and Scale): a BoxShape2D is a BoxShapeGeometry of (HalfExtents.x, HalfExtents.y, h), a
	// CircleShape2D a SphereShapeGeometry of Radius, a PolygonShape2D a ConvexHullShapeGeometry of its points at z = -h and
	// z = h; Scale (x, y, 1) (a circle's uniform), so h is not scaled again; Position (x, y, 0), the rotation of Angle about
	// +Z, UserData kept. Pure. Errors: Validation
	// "PHYSICS_INVALID_SHAPE: collider <i>: <reason>" for no collider, a non-finite value, a non-positive dimension, a polygon of
	// fewer than 3 points, a circle under a non-uniform scale; PhysicsShape::Create checks the rest when the caller builds it.
	[[nodiscard]] Result<BodyShapeDescription> MakeBody2DShape(const Body2DShapeDescription& description);

	// What a 2D world is created with.
	struct Physics2DWorldSpecification
	{
		// Metres per second squared along X and Y (Physics2DSettings.Gravity). Finite, each at most MaxPhysicsGravity.
		glm::vec2 Gravity = glm::vec2(0.0f, -9.81f);
		// The project's layers and collision matrix (PhysicsSettings, shared with the 3D world).
		PhysicsLayerTable Layers{};
		PhysicsWorldLimits Limits{};
	};

	// Everything a 2D body is created from: BodyDescription's members in the plane (its rules and ranges apply), with
	// FixedRotation choosing Physics2DTranslationDofs instead of Physics2DDofs for a Dynamic body. EnhancedInternalEdgeRemoval
	// is passed through (§9.2 "Seams": a circle rolling or a box sliding across the merged boxes of one compound, such as
	// C21.2's tilemap collider, crosses their seams without ghost contacts).
	struct Body2DDescription
	{
		// Required: a shape MakeBody2DShape laid in the plane (PhysicsShape::Create of its result).
		Ref<const PhysicsShape> Shape{};
		PhysicsMotionType MotionType = PhysicsMotionType::Dynamic;
		Physics2DPose Pose{};
		uint32_t Layer = 0;
		bool IsSensor = false;
		bool CollideKinematicVsNonDynamic = false;
		PhysicsMotionQuality MotionQuality = PhysicsMotionQuality::Discrete;
		bool FixedRotation = false;
		float Mass = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		float MaxLinearVelocity = 500.0f;
		float MaxAngularVelocity = 47.12f;
		bool EnhancedInternalEdgeRemoval = false;
		bool AllowSleeping = true;
		glm::vec2 LinearVelocity = glm::vec2(0.0f); // m/s
		float AngularVelocity = 0.0f;               // rad/s about +Z
		uint64_t UserData = 0;
		uint64_t CollisionGroup = 0;
	};

	class Physics2DWorld
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class Physics2DWorld;
		};

		// Use Create.
		explicit Physics2DWorld(ConstructionKey key);
		// Destroys every body still in the world.
		~Physics2DWorld();

		Physics2DWorld(const Physics2DWorld&) = delete;
		Physics2DWorld& operator=(const Physics2DWorld&) = delete;

		// A world with `specification`: a PhysicsWorld with gravity (x, y, 0). Errors: PhysicsWorld::Create's.
		[[nodiscard]] static Result<Scope<Physics2DWorld>> Create(const Physics2DWorldSpecification& specification);

		// --- Bodies (PhysicsWorld's rules) ------------------------------------------------------------------------------
		// A Dynamic body gets Physics2DDofs (Physics2DTranslationDofs with FixedRotation). Errors: PhysicsWorld::CreateBody's.
		[[nodiscard]] Result<BodyHandle> CreateBody(const Body2DDescription& description);
		void DestroyBody(BodyHandle body);
		[[nodiscard]] bool IsBodyValid(BodyHandle body) const;
		[[nodiscard]] uint64_t GetUserData(BodyHandle body) const;
		[[nodiscard]] PhysicsMotionType GetMotionType(BodyHandle body) const;
		[[nodiscard]] bool IsSensor(BodyHandle body) const;
		[[nodiscard]] uint32_t GetLayer(BodyHandle body) const;
		[[nodiscard]] Ref<const PhysicsShape> GetShape(BodyHandle body) const;
		[[nodiscard]] Status SetShape(BodyHandle body, const Ref<const PhysicsShape>& shape);
		[[nodiscard]] uint32_t GetSubShapeCollider(BodyHandle body, uint32_t subShape) const;

		// --- Poses and motion, in the plane -------------------------------------------------------------------------------
		[[nodiscard]] Physics2DPose GetPose(BodyHandle body) const;
		void SetPose(BodyHandle body, const Physics2DPose& pose, bool activate);
		void MoveKinematic(BodyHandle body, const Physics2DPose& target, float deltaTime);
		// Dynamic bodies. Forces and impulses along X and Y; torques and angular impulses about Z (N m, N m s).
		void AddForce(BodyHandle body, const glm::vec2& force);
		void AddForceAtPosition(BodyHandle body, const glm::vec2& force, const glm::vec2& position);
		void AddTorque(BodyHandle body, float torque);
		void AddImpulse(BodyHandle body, const glm::vec2& impulse);
		void AddAngularImpulse(BodyHandle body, float angularImpulse);
		// m/s along X and Y; rad/s about Z.
		[[nodiscard]] glm::vec2 GetLinearVelocity(BodyHandle body) const;
		void SetLinearVelocity(BodyHandle body, const glm::vec2& velocity);
		[[nodiscard]] float GetAngularVelocity(BodyHandle body) const;
		void SetAngularVelocity(BodyHandle body, float velocity);
		[[nodiscard]] bool IsSleeping(BodyHandle body) const;
		void WakeUp(BodyHandle body);

		// --- Queries, in the plane z = 0 ----------------------------------------------------------------------------------
		// A ray from `origin` along the unit `direction` (both in the plane) up to `maxDistance` (> 0). Hits' points and normals
		// lie in the plane (z = 0).
		[[nodiscard]] std::optional<PhysicsQueryHit> Raycast(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
			const PhysicsQueryFilter& filter) const;
		[[nodiscard]] std::vector<PhysicsQueryHit> RaycastAll(const glm::vec2& origin, const glm::vec2& direction, float maxDistance,
			const PhysicsQueryFilter& filter) const;
		// Every body sub-shape overlapping `shape` at `pose`, one entry per sub-shape, in no particular order: the box laid as
		// MakeBody2DShape lays a collider (a box of half depth GetPhysics2DHalfDepth), the circle as a sphere, both passed to
		// PhysicsWorld::Overlap.
		[[nodiscard]] std::vector<PhysicsOverlap> Overlap(const Overlap2DShape& shape, const Physics2DPose& pose, const PhysicsQueryFilter& filter) const;
		[[nodiscard]] std::optional<Aabb> GetBodyBounds(BodyHandle body) const;
		[[nodiscard]] std::optional<Aabb> GetSubShapeBounds(BodyHandle body, uint32_t collider) const;

		// --- Stepping -----------------------------------------------------------------------------------------------------
		[[nodiscard]] Status Step(float deltaTime, uint32_t collisionSteps);
		[[nodiscard]] glm::vec2 GetGravity() const;
		[[nodiscard]] Status SetGravity(const glm::vec2& gravity);
		// PhysicsWorld::DrainContactEvents's records, each laid in the plane: its point at z = 0 and its normal in the plane
		// (z = 0, unit length); a Removed record is left as it is.
		[[nodiscard]] std::vector<ContactEvent> DrainContactEvents();

		[[nodiscard]] const PhysicsLayerTable& GetLayers() const;
		[[nodiscard]] PhysicsWorldStats GetStats() const;
		// The Jolt world it wraps (tests, debug draw and W21.2's physics2d.bodyInfo read it; never a way off the plane); null
		// only before Create finished.
		[[nodiscard]] const PhysicsWorld* GetWorld() const;
	private:
		// The wrapped PhysicsWorld (Physics/Physics2DWorld.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
