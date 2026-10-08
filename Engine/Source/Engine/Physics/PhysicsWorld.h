#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/ContactBuffer.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsQueries.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <vector>

// The physics world (Architecture §9.1): one Jolt PhysicsSystem with its temporary allocator, layer filters and contact
// listener, one per play session, owned by Scene/PhysicsSystem. It knows nothing about entities or the ECS: its API is
// expressed in engine types (bodies by BodyHandle, shapes as PhysicsShape, poses and vectors as glm), and the 64-bit user
// data of each body is where its owner keeps the entity's UUID (§9.2).
//
// Nothing invalid reaches Jolt (§9.1): every call checks its values (finite, in range, a live handle of this world, the
// right motion type) and refuses bad ones with a Result or, for the void mutators, ignores them after an assert in Debug
// and Release (callers validate external input first, as Scene/PhysicsSystem does; an assert is never the only guard).
// Content errors carry a PHYSICS_* code at the start of their message (Physics/PhysicsDiagnostics.h). Settings Jolt would
// assert on are made safe instead (BodyDescription), and two bodies whose shapes both hold a triangle mesh never meet:
// Jolt has no mesh-versus-mesh collision and asserts on such a pair, which a mesh sensor and a Kinematic mesh body could
// otherwise form (sensors detect kinematic bodies), so the world's filters drop it before the narrow phase.
//
// Determinism (§9.1): with JPH_CROSS_PLATFORM_DETERMINISTIC, the precise floating-point model and the same calls in the
// same order, the world's state after Step is identical in every configuration and for every worker thread count
// (PhysicsEngine::SetWorkerThreadCount). Bodies get Jolt's body IDs in creation order, so a caller that creates them in
// canonical order (§5.1) gets the same handles every run. Contact callbacks arrive in a non-deterministic order; the buffer
// is drained unsorted and the consumer sorts.
//
// Threading: main thread only (§4.11); Step runs Jolt's jobs on the process's JobSystemThreadPool and returns when they are
// done. Not copyable or movable. Every live world counts in PhysicsEngine::GetLiveWorldCount, every live body in
// GetLiveBodyCount; destroying the world destroys its bodies.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 3).

namespace Engine {

	// What a world is created with.
	struct PhysicsWorldSpecification
	{
		// Metres per second squared (PhysicsSettings.Gravity). Finite.
		glm::vec3 Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		// The project's layers and collision matrix (§9.2), copied into the world's filters.
		PhysicsLayerTable Layers{};
		PhysicsWorldLimits Limits{};
	};

	// Everything a body is created from (Jolt's BodyCreationSettings, §9.2 "Body settings"). Every float finite. Settings
	// that Jolt asserts on are made safe before they reach it (§9.1), as each member says: AllowedDofs and Mass act on
	// Dynamic bodies only, and starting velocities are clamped.
	struct BodyDescription
	{
		// Required.
		Ref<const PhysicsShape> Shape{};
		PhysicsMotionType MotionType = PhysicsMotionType::Dynamic;
		// The body origin's world pose; Rotation unit.
		PhysicsPose Pose{};
		// The project layer (< the table's layer count); the object layer adds the kind (§9.2).
		uint32_t Layer = 0;
		// A Jolt sensor (§9.2 "Triggers"): MotionType must be Kinematic (or Static, which Jolt allows but which only sees
		// active bodies; Scene/PhysicsSystem never creates those), else PHYSICS_DYNAMIC_TRIGGER. Its object layer kind is
		// Sensor, and it is kept active (never sleeps), so it detects sleeping bodies.
		bool IsSensor = false;
		// Jolt's mCollideKinematicVsNonDynamic: sensors set it so they detect kinematic platforms and character inner bodies
		// (§9.2, §9.6).
		bool CollideKinematicVsNonDynamic = false;
		PhysicsMotionQuality MotionQuality = PhysicsMotionQuality::Discrete;
		// Dynamic bodies: at least one, else PHYSICS_ALL_DOFS_LOCKED. Every other body gets PhysicsDofs::All whatever this
		// holds (Jolt asserts on a Kinematic body that cannot move).
		PhysicsDofs AllowedDofs = PhysicsDofs::All;
		// Dynamic bodies, kg, > 0: overrides the shape's mass, inertia scaled to it (EOverrideMassProperties::
		// CalculateInertia). Ignored otherwise: a Kinematic body gets fixed mass properties instead of its shape's
		// (EOverrideMassProperties::MassAndInertiaProvided, 1 kg with a unit sphere's inertia; Jolt never uses a kinematic
		// body's mass in contacts), so a triangle mesh, whose MeshShape has no mass, is valid on it; a Static body has none.
		float Mass = 1.0f;
		float Friction = 0.5f;       // >= 0
		float Restitution = 0.0f;    // [0, 1]
		float LinearDamping = 0.05f; // >= 0
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		float MaxLinearVelocity = 500.0f;  // m/s, >= 0
		float MaxAngularVelocity = 47.12f; // rad/s, >= 0
		bool AllowSleeping = true;
		// Jolt's mEnhancedInternalEdgeRemoval (§9.2 "Seams"): set on rolling bodies.
		bool EnhancedInternalEdgeRemoval = false;
		// The world-space velocities the body starts with (Dynamic and Kinematic; Static bodies ignore them), clamped to
		// MaxLinearVelocity and MaxAngularVelocity as Jolt's BodyInterface::SetLinearVelocity clamps (Jolt asserts on a
		// longer starting velocity).
		glm::vec3 LinearVelocity = glm::vec3(0.0f);
		glm::vec3 AngularVelocity = glm::vec3(0.0f);
		// The owner's data, returned by GetUserData: Scene/PhysicsSystem stores the owning entity's UUID.
		uint64_t UserData = 0;
		// Bodies whose CollisionGroup is the same non-zero value never collide, and sensors never report them to each other
		// (a Jolt GroupFilter the world installs; queries are not affected); 0 is no group. Scene/PhysicsSystem gives every
		// body its plan's group (PhysicsBodyPlan::CollisionGroup: the nearest body or character owner at or above it), so a
		// trigger attached to a moving body or a character never reports that body or character (§5.3).
		uint64_t CollisionGroup = 0;
	};

	// Counts of a world, for stats.get (§13.7) and the limit diagnostics.
	struct PhysicsWorldStats
	{
		uint32_t BodyCount = 0;
		uint32_t ActiveBodyCount = 0;
		uint32_t BodyPairCount = 0;
		uint32_t ContactConstraintCount = 0;
	};

	class PhysicsWorld
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PhysicsWorld;
		};

		// Use Create.
		explicit PhysicsWorld(ConstructionKey key);
		// Destroys every body still in the world.
		~PhysicsWorld();

		PhysicsWorld(const PhysicsWorld&) = delete;
		PhysicsWorld& operator=(const PhysicsWorld&) = delete;

		// A world with `specification` (§9.1: PhysicsSystem::Init with the limits, a TempAllocatorImpl of
		// Limits.TempAllocatorBytes, the gravity, the layer filters over the table, the contact listener over the world's
		// ContactBuffer). Errors: InvalidState when the PhysicsEngine is not initialized (ProcessContext's Physics step);
		// InvalidArgument for a non-finite gravity or a zero limit.
		[[nodiscard]] static Result<Scope<PhysicsWorld>> Create(const PhysicsWorldSpecification& specification);

		// --- Bodies ---------------------------------------------------------------------------------------------------
		// Creates and adds a body (activated unless it is Static), with BodyDescription's safety rules applied (a Kinematic
		// body takes any shape, triangle meshes included, and every degree of freedom; starting velocities are clamped; a
		// body holding a mesh never meets another one, see the file comment).
		// Errors, with nothing created: InvalidArgument for a missing shape, a non-finite value, a non-unit rotation, a value
		// outside its documented range or a layer outside the table; Validation "PHYSICS_DYNAMIC_TRIGGER: ..." for a Dynamic
		// sensor, "PHYSICS_ALL_DOFS_LOCKED: ..." for a Dynamic body without a degree of freedom, "PHYSICS_NONCONVEX_DYNAMIC:
		// ..." for a Dynamic body whose shape holds a mesh; InvalidState "PHYSICS_LIMIT_EXCEEDED: ..." when the world holds
		// Limits.MaxBodies bodies.
		[[nodiscard]] Result<BodyHandle> CreateBody(const BodyDescription& description);
		// Removes and destroys a body (Jolt reports no OnContactRemoved for its pairs: the session synthesizes exits, §9.4).
		// A handle that names no live body of this world is a programmer error (asserted) and ignored.
		void DestroyBody(BodyHandle body);
		// Whether `body` names a live body of this world.
		[[nodiscard]] bool IsBodyValid(BodyHandle body) const;
		// The body's description data the world keeps: GetUserData is BodyDescription::UserData; GetMotionType its motion
		// type; IsSensor its sensor flag; GetLayer its project layer. `body` live (asserted; the invalid answers 0, Static,
		// false, 0 otherwise).
		[[nodiscard]] uint64_t GetUserData(BodyHandle body) const;
		[[nodiscard]] PhysicsMotionType GetMotionType(BodyHandle body) const;
		[[nodiscard]] bool IsSensor(BodyHandle body) const;
		[[nodiscard]] uint32_t GetLayer(BodyHandle body) const;
		// The body's shape (the one it was created with, or the last SetShape).
		[[nodiscard]] Ref<const PhysicsShape> GetShape(BodyHandle body) const;
		// Replaces the body's shape (a compound rebuilt after a child collider changed, §9.2), keeping its handle, pose,
		// velocities and contacts' bookkeeping to Jolt (its contacts are re-evaluated at the next step, and Jolt reports the
		// sub-shape contacts that ended and began as usual). A Dynamic body keeps its Mass, its inertia recomputed from the
		// new shape and scaled to it; other bodies keep the mass properties CreateBody gave them (never the shape's, so a
		// Kinematic body may take a mesh). Errors: InvalidArgument for a dead handle or a missing shape; Validation
		// "PHYSICS_NONCONVEX_DYNAMIC: ..." for a mesh on a Dynamic body.
		[[nodiscard]] Status SetShape(BodyHandle body, const Ref<const PhysicsShape>& shape);

		// --- Poses and motion (§9.3) ----------------------------------------------------------------------------------
		// The body origin's world pose (Jolt's position and rotation; the centre of mass is internal).
		[[nodiscard]] PhysicsPose GetPose(BodyHandle body) const;
		// Teleports the body (SetPositionAndRotation) and, when `activate`, wakes it; velocities are kept (§9.3: "keeping its
		// velocity unless the script also set it").
		void SetPose(BodyHandle body, const PhysicsPose& pose, bool activate);
		// Kinematic bodies: moves the body so it reaches `target` at the end of a step of `deltaTime` seconds (> 0), with the
		// velocities that carry resting bodies along (Jolt's MoveKinematic; §9.3 moving platforms, implicit sensor bodies).
		void MoveKinematic(BodyHandle body, const PhysicsPose& target, float deltaTime);
		// Dynamic bodies (others: asserted and ignored). Forces and torques act during the next step and are then cleared;
		// impulses change the velocity at once. Each wakes the body.
		void AddForce(BodyHandle body, const glm::vec3& force);
		void AddForceAtPosition(BodyHandle body, const glm::vec3& force, const glm::vec3& position);
		void AddTorque(BodyHandle body, const glm::vec3& torque);
		void AddImpulse(BodyHandle body, const glm::vec3& impulse);
		void AddAngularImpulse(BodyHandle body, const glm::vec3& angularImpulse);
		// World-space velocities (m/s, rad/s) of Kinematic and Dynamic bodies; zero for Static ones. Setting one wakes the
		// body and clamps the velocity to the body's maximum, as Jolt's BodyInterface does (Static bodies: asserted and
		// ignored).
		[[nodiscard]] glm::vec3 GetLinearVelocity(BodyHandle body) const;
		void SetLinearVelocity(BodyHandle body, const glm::vec3& velocity);
		[[nodiscard]] glm::vec3 GetAngularVelocity(BodyHandle body) const;
		void SetAngularVelocity(BodyHandle body, const glm::vec3& velocity);
		// Whether a non-static body is asleep (inactive); Static bodies are never active and report true.
		[[nodiscard]] bool IsSleeping(BodyHandle body) const;
		void WakeUp(BodyHandle body);

		// --- Queries (§9.5; main thread, between steps) ---------------------------------------------------------------
		// The closest hit of `ray` (Jolt's CastRay through the NarrowPhaseQuery), or nullopt. Backface hits are ignored
		// (rays starting inside a convex shape hit it at distance 0).
		[[nodiscard]] std::optional<PhysicsQueryHit> Raycast(const PhysicsRay& ray, const PhysicsQueryFilter& filter) const;
		// Every hit along `ray`, one per body sub-shape, in no particular order (the caller sorts).
		[[nodiscard]] std::vector<PhysicsQueryHit> RaycastAll(const PhysicsRay& ray, const PhysicsQueryFilter& filter) const;
		// The first hit of `shape` (a primitive: box, sphere or capsule) swept from `start` along the unit `direction` up to
		// `maxDistance` (> 0) (Jolt's CastShape; §11.5 Physics.SphereCast), or nullopt. A shape that already overlaps
		// something at `start` hits it at distance 0.
		[[nodiscard]] std::optional<PhysicsQueryHit> ShapeCast(const PhysicsShapeGeometry& shape, const PhysicsPose& start, const glm::vec3& direction,
			float maxDistance, const PhysicsQueryFilter& filter) const;
		// Every body sub-shape overlapping `shape` (a primitive) at `pose` (Jolt's CollideShape), one entry per sub-shape, in
		// no particular order.
		[[nodiscard]] std::vector<PhysicsOverlap> Overlap(const PhysicsShapeGeometry& shape, const PhysicsPose& pose, const PhysicsQueryFilter& filter) const;
		// The world AABB of the body's whole shape; nullopt for a dead handle.
		[[nodiscard]] std::optional<Aabb> GetBodyBounds(BodyHandle body) const;
		// The world AABB of the body's sub-shapes whose user data is `collider` (§9.5 GetColliderBounds: a collider's own
		// shape, whether a standalone body or part of a compound); nullopt when the body is dead or has no such collider.
		[[nodiscard]] std::optional<Aabb> GetSubShapeBounds(BodyHandle body, uint32_t collider) const;

		// --- Stepping (§9.3) ------------------------------------------------------------------------------------------
		// Advances the world by `deltaTime` seconds (> 0, finite) in `collisionSteps` (>= 1) collision steps (Jolt's
		// PhysicsSystem::Update). Scene/PhysicsSystem passes max(1, ceil(60 / FixedHz)), computed in integers, which is §9.3's
		// max(1, ceil(fixedDt * 60)) without the floating-point rounding of 1/FixedHz * 60. Contact records go to the
		// world's ContactBuffer. Errors: InvalidArgument for a bad delta or step count; InvalidState
		// "PHYSICS_LIMIT_EXCEEDED: ..." when Jolt reports a full body pair cache, manifold cache or contact constraint
		// buffer (the step still ran; contacts beyond the limit were dropped).
		[[nodiscard]] Status Step(float deltaTime, uint32_t collisionSteps);
		[[nodiscard]] glm::vec3 GetGravity() const;
		// Errors: InvalidArgument for a non-finite gravity.
		[[nodiscard]] Status SetGravity(const glm::vec3& gravity);
		// The contact records the listeners appended since the last drain (ContactBuffer::Drain).
		[[nodiscard]] std::vector<ContactEvent> DrainContactEvents();
		// The buffer the world's listeners append to; CharacterController's listener appends here too (§9.6).
		[[nodiscard]] ContactBuffer& GetContactBuffer();

		[[nodiscard]] const PhysicsLayerTable& GetLayers() const;
		[[nodiscard]] const PhysicsWorldLimits& GetLimits() const;
		[[nodiscard]] PhysicsWorldStats GetStats() const;
	private:
		// The Jolt objects (Physics/Private/PhysicsWorldState.h, shared by the world's .cpp files and CharacterController).
		struct State;
	private:
		Scope<State> m_State;
	private:
		friend class CharacterController;
	};

}
