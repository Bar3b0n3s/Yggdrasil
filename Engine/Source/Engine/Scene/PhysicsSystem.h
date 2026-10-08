#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The play session's physics (Architecture §9.2 to §9.6, §5.7 steps 5 to 8): the bridge between a runtime scene and its
// PhysicsWorld. It creates, rebuilds and removes bodies and characters from the scene's components under the composition
// rules (PhysicsComposition.h), keeps them in step with the scene around each PhysicsWorld::Step, turns the world's raw
// contacts into sorted per-entity events, answers the scene-level queries of scripts and automation, and reports what
// cannot be simulated as diagnostics, never asserts (§9.1).
//
// The step (§5.7; PlaySession calls each hook in its phase, in Play and Simulate alike):
//   - PreStep (step 5, right after TransformSystem::Update, so world matrices hold every Transform write made earlier in
//     the tick), in canonical order of the owners:
//       1. Rebuilds (§9.2: EnTT signals and the scene's structure mark owners dirty; bodies are created in canonical order,
//          so component order never matters). A body or character is created, rebuilt or removed when, since the last
//          PreStep: its owner's RigidBody or CharacterController changed, was added or removed; one of its colliders (its
//          own or a gathered one) changed, was added or removed; the hierarchy changed above or below a collider (a
//          reparent moves colliders into or out of a compound); the world scale of the owner or of a gathered collider's
//          entity changed (scale is baked into shapes); the MeshRenderer.Mesh of an entity whose MeshCollider has no Mesh
//          changed; a collider's mesh got a new asset version (a hot reload, §7.5); an entity became effectively enabled;
//          or a gathered collider moved relative to its body. A rebuild caused by colliders alone (their components,
//          the hierarchy, scales, meshes, moves) that leaves the body's motion type and sensor flag as they were replaces
//          its shape in place (PhysicsWorld::SetShape: handle, pose, velocities and Jolt's contact bookkeeping kept); any
//          other rebuild (a RigidBody or CharacterController change) destroys the body and creates it again with its
//          current linear and angular velocity. The RigidBody's Initial* velocities apply only to a Dynamic body created
//          for an owner that had none (session start, a new component or entity, a re-enabled entity); a Kinematic body
//          follows its Transform. An entity that became disabled or is pending
//          destruction keeps the body it has until FlushDestroyed (step 8), which closes its pairs, and gets none
//          created; PreStep never removes it.
//       2. Pairs across rebuilds. Active pairs are keyed by the two owners' UUIDs and their kind (collision or trigger),
//          never by body handle. When PreStep destroys a body (a rebuild, or a removal: the RigidBody or the last collider
//          removed, a body refused after an edit), each active pair of that owner loses its sub-shape counts and becomes
//          unconfirmed, and PreStep wakes the partners (sensors never sleep), so the step reports every contact that
//          still touches. At that tick's PostStep, an unconfirmed pair that a contact re-established stays active without
//          a new enter; one that none re-established ends with a synthesized exit, dispatched with that PostStep's events
//          in their sorted order.
//       3. Kinematic bodies (Static sensor RigidBodies included) and implicit sensor bodies MoveKinematic to their
//          entity's world pose over the fixed delta, with the velocities that carry resting bodies (§9.3); one whose
//          entity carries InterpolationResetTag (teleported in steps 1 to 4 of this tick: Teleport,
//          PlaySession::MarkTeleported) is placed with PhysicsWorld::SetPose instead, at rest, so a teleport never flings
//          what rests on it.
//       4. Teleports (§9.3 "Transform write in OnFixedUpdate teleports the body in the same tick"), compared bit for bit
//          against values the engine wrote, so a body at rest is never teleported by its own write-back: a Dynamic body
//          when its owner's local TransformComponent Translation or Rotation differs from what PostStep last wrote there
//          (or what the body was created from), so a moving parent never drags it (PHYSICS_DYNAMIC_UNDER_MOVING_PARENT);
//          a Static body when its owner's world matrix differs from the one it was created or last teleported at; a
//          character as a Dynamic body, moved with CharacterController::SetPose. A teleported body keeps its velocity
//          unless it was set too; a Dynamic one is woken, and a Static one wakes the bodies around its old and new place.
//          A pose that is not IsPlaceablePhysicsPose (a Transform written beyond MaxPhysicsCoordinate) is not applied:
//          the entity's local Translation and Rotation go back to the body's (the values PostStep last wrote, or those
//          the body was placed from; a Kinematic body's current pose, where it then stops).
//       5. Characters run CharacterController::Update with the velocity of their last MoveCharacter, which it consumes.
//   - Step (step 6): PhysicsWorld::Step(fixedDelta, max(1, ceil(60 / FixedHz))).
//   - PostStep (step 7): Dynamic bodies that are active or have a parent write their world pose back as their entity's
//     local Transform (through the parent's current inverse, so the entity stays where the body is even when the parent
//     moved), and characters theirs, visiting bodies in canonical order, never in Jolt's active-body order; then the
//     contacts are drained, mapped to owners, collapsed into body-pair begin and end events with a per-pair reference
//     count of sub-shape contacts (net over the step's records: a pair whose count rises from zero enters, one whose count
//     falls to zero exits), sorted by (UUID a, UUID b, type), and dispatched (§9.4 steps 1 to 4, PhysicsEvent,
//     IPhysicsEventListener). Sleeping never ends a pair: a Removed record for a contact whose bodies fell asleep leaves
//     it dormant (it still counts), and a dormant contact is dropped only after a step that began with one of its bodies
//     awake and did not report it again.
//   - FlushDestroyed (step 8, and the frame phase's destroy flush, before Scene::FlushPendingDestroys and after the
//     scripts' OnDestroy): every active pair involving an entity marked for destruction (PendingDestroyTag) or effectively
//     disabled is closed, each surviving side that received the pair's enter getting a synthesized exit (§9.4 "Exit on
//     destroy, disable and character contacts"), dispatched in sorted order; then those entities' bodies and characters
//     are removed (a re-enabled entity gets its body back at the next PreStep). The listener may destroy or disable more
//     entities during that dispatch, so the flush repeats its close, dispatch and remove pass until a pass finds nothing
//     left to do; each entity is handled once, which bounds the passes by the entity count (M13's OnDestroy for entities
//     destroyed there joins the same loop). When it returns, no entity pending destruction or disabled has a body, a
//     character or an active pair.
//
// Determinism (§9.1): bodies, characters, write-back, events and the state hash all follow canonical order or UUID order;
// nothing iterates an unordered container to produce output; results do not depend on the worker thread count.
//
// Threading: main thread only (§4.11). Not copyable or movable. The system reads and writes its scene directly (a runtime
// system, §5.1), so its writes do not count in the scene's revision.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decisions 11 to 13).

namespace Engine {

	class AssetManager;
	class Scene;
	class XXH64Hasher;

	// What a system is created with. The play session fills it from its specification.
	struct PhysicsSystemSpecification
	{
		// Required, outliving the system (a documented back-reference, §4.7): the play session's runtime scene.
		Scene* RuntimeScene = nullptr;
		// The project's PhysicsSettings (§6.1), copied member by member (Scene does not include Project, §3).
		glm::vec3 Gravity = glm::vec3(0.0f, -9.81f, 0.0f);
		std::vector<std::string> Layers = { std::string(DefaultPhysicsLayerName) };
		std::vector<std::vector<std::string>> Collisions = { { std::string(DefaultPhysicsLayerName), std::string(DefaultPhysicsLayerName) } };
		// Simulation.FixedHz (>= 1): the collision steps of each world step.
		uint32_t FixedHz = 60;
		// The session's asset manager, outliving the system: MeshCollider meshes. Null makes every mesh collider a
		// PHYSICS_INVALID_SHAPE diagnostic.
		AssetManager* Assets = nullptr;
		// The world's capacities (§9.1 defaults; tests lower them).
		PhysicsWorldLimits Limits{};
	};

	// The four callbacks of §9.4.
	enum class PhysicsEventType : uint8_t
	{
		CollisionEnter,
		CollisionExit,
		TriggerEnter,
		TriggerExit
	};

	// The contact of an event, from the receiving entity's side (§9.4: contact = {Point, Normal, RelativeSpeed, Collider,
	// OtherCollider}). Collider and OtherCollider are the collider entities of the first sub-shape pair that touched,
	// resolved through the sub-shape user data (§9.2); they differ from the body owners for compounds. Point, Normal and
	// RelativeSpeed are those of that first contact for CollisionEnter, and zero (Normal (0, 1, 0)) for the other types.
	struct PhysicsContact
	{
		glm::vec3 Point = glm::vec3(0.0f);              // world space
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f); // unit, pointing from the receiving body towards the other one
		float RelativeSpeed = 0.0f;                     // m/s along Normal, positive when approaching
		UUID Collider{};                                // the receiving body's collider entity
		UUID OtherCollider{};                           // the other body's collider entity

		bool operator==(const PhysicsContact&) const = default;
	};

	// One callback for one entity of a body pair. Pair events are per body pair, never per sub-shape: a ball rolling from
	// one piece of a compound track to the next stays in contact with the same body and gets no new CollisionEnter (§9.4).
	struct PhysicsEvent
	{
		PhysicsEventType Type = PhysicsEventType::CollisionEnter;
		// The body owner receiving the callback (scripts on it run OnCollision* / OnTrigger*, M13).
		UUID Self{};
		// The partner's body owner (`other` of §9.4). For a synthesized exit it may be destroyed (Scene::FindEntityByID no
		// longer finds it: `other:IsValid() == false`) or disabled (`other:IsActive() == false`).
		UUID Other{};
		PhysicsContact Contact{};
		// An exit the system made instead of Jolt: at a destroy or disable flush (FlushDestroyed), or at the PostStep after
		// PreStep destroyed a body of the pair and no contact re-established it (see the file comment).
		bool Synthesized = false;
		// The tick whose PostStep or flush produced it.
		uint64_t Tick = 0;

		bool operator==(const PhysicsEvent&) const = default;
	};

	// Receives the sorted events and the run-time diagnostics (the interface scripting consumes in M13: the ScriptEngine
	// dispatches each event to the scripts on Self, and turns diagnostics about refused bodies into script errors, §9.1).
	// Implementations live outside Scene; tests record them.
	class IPhysicsEventListener
	{
	public:
		virtual ~IPhysicsEventListener() = default;

		// Called on the main thread from PostStep and FlushDestroyed, once per event, in this order: pair events sorted by
		// (lower UUID, higher UUID, PhysicsEventType), and for each pair event first the callback whose Self is the lower
		// UUID, then the other. What is dropped (§9.4 step 3):
		//   - the events of a pair with an entity that is already pending destruction or effectively disabled when the
		//     dispatch starts (destroyed or disabled earlier in the tick, while its body still simulated): an enter is not
		//     delivered and the pair is not recorded as active, so it never gets an exit; an exit is left to FlushDestroyed,
		//     which closes the pair with synthesized exits;
		//   - a callback whose Self was destroyed or disabled by an earlier callback of the same dispatch; its partner's
		//     callback is still delivered (its Other is then invalid or inactive, as in a synthesized exit), and that
		//     partner gets its exit at the next flush.
		// Each side of a pair that got the enter gets exactly one exit, unless its own entity is destroyed or disabled first;
		// a side that did not get the enter gets no exit. The listener may change the scene (components, entity creation,
		// deferred destruction, §5.7) and call the PhysicsSystem's body, character and query functions; it must not call
		// PreStep, Step, PostStep, FlushDestroyed or SetEventListener (asserted). Destroying an entity here is safe (§9.7
		// "destroy inside a callback is safe"): it is deferred to the next flush, which synthesizes its exits, also when
		// that flush is the one dispatching.
		virtual void OnPhysicsEvent(const PhysicsEvent& event) = 0;

		// Called on the main thread once for each diagnostic the system raises while the listener is set (PreStep's
		// creations and rebuilds, Step's limits), in the order raised, right after it was logged; the rules of
		// OnPhysicsEvent apply. The diagnostics Create raised precede any listener: GetDiagnostics holds them. The default
		// ignores them.
		virtual void OnPhysicsDiagnostic(const PhysicsDiagnostic& /*diagnostic*/) {}
	};

	// A ray or shape-cast hit (§9.5, §11.5 RaycastHit {Entity, Body, Point, Normal, Distance}).
	struct PhysicsRaycastHit
	{
		UUID Entity{}; // the collider entity that was hit
		UUID Body{};   // the owner of the body it belongs to
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		float Distance = 0.0f;

		bool operator==(const PhysicsRaycastHit&) const = default;
	};

	// One collider that overlaps a query shape.
	struct PhysicsOverlapHit
	{
		UUID Entity{}; // the collider entity
		UUID Body{};   // the owner of its body

		bool operator==(const PhysicsOverlapHit&) const = default;
	};

	// A character's state (§9.6 CharacterController:IsGrounded, GetGroundNormal, GetVelocity).
	struct PhysicsCharacterState
	{
		bool IsGrounded = false;
		glm::vec3 GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);
	};

	// One active contact pair of a body (physics.bodyInfo "contacts", §13.5).
	struct PhysicsContactInfo
	{
		UUID Other{};         // the partner's body owner
		UUID Collider{};      // this body's collider entity of the first sub-shape pair that touched
		UUID OtherCollider{}; // the partner's
		bool IsTrigger = false;
		uint64_t SinceTick = 0; // the tick whose PostStep reported the enter
	};

	// What physics.bodyInfo reports about one body (§13.5: velocity, sleeping, contacts, layer). Not named after the method:
	// Automation::PhysicsBodyInfo, its handler, would hide the type inside namespace Automation.
	struct PhysicsBodyReport
	{
		UUID Owner{};
		PhysicsBodyOrigin Origin = PhysicsBodyOrigin::RigidBody;
		PhysicsMotionType MotionType = PhysicsMotionType::Static; // PhysicsBodyPlan::MotionType
		bool IsSensor = false;
		uint32_t Layer = 0;
		std::string LayerName{};
		// The collider entities in sub-shape user data order (empty for characters).
		std::vector<UUID> Colliders{};
		// The body origin's world pose; a character's base (CharacterController::GetPose), not its inner body's centre.
		PhysicsPose Pose{};
		glm::vec3 LinearVelocity = glm::vec3(0.0f);
		glm::vec3 AngularVelocity = glm::vec3(0.0f);
		bool IsSleeping = false;
		Aabb Bounds{}; // world space
		// The body's active pairs, sorted by Other.
		std::vector<PhysicsContactInfo> Contacts{};
		// Characters only.
		std::optional<PhysicsCharacterState> Character{};
	};

	struct PhysicsSystemStats
	{
		uint32_t BodyCount = 0;       // bodies in the world, character inner bodies included
		uint32_t ActiveBodyCount = 0; // awake bodies
		uint32_t CharacterCount = 0;
		uint32_t ContactPairCount = 0; // active body pairs (§9.4)
	};

	class PhysicsSystem
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PhysicsSystem;
		};

		// Use Create.
		explicit PhysicsSystem(ConstructionKey key);
		// Removes every body and character (no events), then destroys the world.
		~PhysicsSystem();

		PhysicsSystem(const PhysicsSystem&) = delete;
		PhysicsSystem& operator=(const PhysicsSystem&) = delete;

		// The physics of `specification.RuntimeScene`: the layer table (PhysicsLayerTable::Create), the PhysicsWorld, and
		// every body and character of the scene's current components, created in canonical order (§5.6 "Session setup:
		// physics bodies are created in canonical order"), so physics.bodyInfo and queries work at tick 0. Content problems
		// become diagnostics (GetDiagnostics), never errors. Errors: InvalidArgument for a missing scene, a FixedHz of 0 or a
		// Gravity component that is not finite or above MaxPhysicsGravity in magnitude (the project's Physics.Gravity); the
		// layer table's Validation errors; PhysicsWorld::Create's errors.
		[[nodiscard]] static Result<Scope<PhysicsSystem>> Create(const PhysicsSystemSpecification& specification);

		// --- The step (see the file comment) ------------------------------------------------------------------------------
		void PreStep(const SimStep& step);
		// A step error (PHYSICS_LIMIT_EXCEEDED) is recorded as a diagnostic and logged once; the session keeps running.
		void Step(const SimStep& step);
		void PostStep(const SimStep& step);
		// `tick`: the tick the flush belongs to (the session's current tick in the frame phase).
		void FlushDestroyed(uint64_t tick);

		// The listener PostStep and FlushDestroyed dispatch to; null (the default) dispatches to nobody. Not owned; it must
		// outlive the system or be replaced first.
		void SetEventListener(IPhysicsEventListener* listener);
		// The events of the last PostStep and of the flushes since, in dispatch order, dropped ones excluded (what tests and
		// observability read; cleared at the start of each PostStep).
		[[nodiscard]] std::span<const PhysicsEvent> GetLastEvents() const;

		// --- Bodies (§11.5 RigidBody; `entity` owns a RigidBody body) ------------------------------------------------------
		// Errors of every function in this group: NotFound "entity <id> has no physics body" when the entity owns no
		// created body (no RigidBody, refused by a diagnostic, or disabled); InvalidArgument for a non-finite or out-of-range
		// value; InvalidState naming the body's motion type when the operation needs another (forces, impulses, torques and
		// setting velocities: Dynamic, since a Kinematic body's velocity comes from its Transform at every step;
		// MoveKinematic: Kinematic).
		[[nodiscard]] Status AddForce(UUID entity, const glm::vec3& force);
		[[nodiscard]] Status AddForceAtPosition(UUID entity, const glm::vec3& force, const glm::vec3& position);
		[[nodiscard]] Status AddTorque(UUID entity, const glm::vec3& torque);
		[[nodiscard]] Status AddImpulse(UUID entity, const glm::vec3& impulse);
		[[nodiscard]] Status AddAngularImpulse(UUID entity, const glm::vec3& angularImpulse);
		[[nodiscard]] Result<glm::vec3> GetLinearVelocity(UUID entity) const;
		[[nodiscard]] Status SetLinearVelocity(UUID entity, const glm::vec3& velocity);
		[[nodiscard]] Result<glm::vec3> GetAngularVelocity(UUID entity) const;
		[[nodiscard]] Status SetAngularVelocity(UUID entity, const glm::vec3& velocity);
		// Kinematic bodies: sets the entity's world pose to the target (through its parent's inverse), so the next PreStep's
		// MoveKinematic carries the body there over the step (§9.6 moving platforms). `rotation` unit.
		[[nodiscard]] Status MoveKinematic(UUID entity, const glm::vec3& position, const glm::quat& rotation);
		// Any body or character: sets the entity's world pose (rotation kept when nullopt) and the body's at once
		// (PhysicsWorld::SetPose, also for a Kinematic body, which is not swept there; CharacterController::SetPose), without
		// interpolation (InterpolationResetTag, as PlaySession::MarkTeleported) and keeping its velocity (§9.3, §11.5
		// RigidBody:Teleport; a respawn sets the velocity to zero separately). A Static body wakes the bodies around its old
		// and new place. InvalidArgument, with nothing changed, when the world position the entity gets under its parent is
		// not within MaxPhysicsCoordinate.
		[[nodiscard]] Status Teleport(UUID entity, const glm::vec3& position, std::optional<glm::quat> rotation);
		[[nodiscard]] Result<bool> IsSleeping(UUID entity) const;
		[[nodiscard]] Status WakeUp(UUID entity);

		// --- Characters (§9.6, §11.5 CharacterController) -------------------------------------------------------------------
		// Sets the velocity of the character's next PreStep update (CharacterController::Update's desired velocity), which
		// is consumed by that update: a script calls Move in every OnFixedUpdate it wants the character to move. Errors:
		// NotFound "entity <id> has no character" when the entity owns no created character (or is disabled); InvalidArgument
		// for a non-finite velocity or one with a component above MaxCharacterSpeed (Physics/CharacterController.h).
		[[nodiscard]] Status MoveCharacter(UUID entity, const glm::vec3& velocity);
		// The state after the last update. Errors: as MoveCharacter.
		[[nodiscard]] Result<PhysicsCharacterState> GetCharacterState(UUID entity) const;

		// --- Queries (§9.5; between steps) ----------------------------------------------------------------------------------
		// Hits report the collider entity and the owning body entity. Results are sorted by distance, then by collider UUID
		// (overlaps, which have no distance: by collider UUID, each collider once). Every query sees sensors (implicit sensor
		// bodies and triggers; Raycast through a checkpoint hits it). Entities pending destruction or disabled are not hit
		// (their bodies stay until FlushDestroyed). Errors of every query: InvalidArgument for a non-finite value, a direction
		// that is not normalizable (length below 1e-6; it is normalized otherwise), a distance <= 0, a radius or half extent
		// outside [PhysicsShape::MinColliderSize / 2, PhysicsShape::MaxColliderSize / 2] (a collider's sizes), or an origin,
		// end point or centre beyond MaxPhysicsCoordinate.
		[[nodiscard]] Result<std::optional<PhysicsRaycastHit>> Raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::vector<PhysicsRaycastHit>> RaycastAll(const glm::vec3& origin, const glm::vec3& direction, float maxDistance,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::optional<PhysicsRaycastHit>> SphereCast(const glm::vec3& origin, float radius, const glm::vec3& direction,
			float maxDistance, PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::vector<PhysicsOverlapHit>> OverlapSphere(const glm::vec3& center, float radius, PhysicsLayerMask layers = AllPhysicsLayers) const;
		[[nodiscard]] Result<std::vector<PhysicsOverlapHit>> OverlapBox(const glm::vec3& center, const glm::vec3& halfExtents, const glm::quat& rotation,
			PhysicsLayerMask layers = AllPhysicsLayers) const;
		// The world AABB of the body `entity` owns (§9.5 GetBodyBounds); nullopt when it owns none or is pending destruction
		// or disabled.
		[[nodiscard]] std::optional<Aabb> GetBodyBounds(UUID entity) const;
		// The world AABB of `entity`'s own collider sub-shapes, whether they form a standalone body or part of an
		// ancestor's compound (§9.5 GetColliderBounds); nullopt when the entity has no collider in a created body, or is
		// pending destruction or disabled.
		[[nodiscard]] std::optional<Aabb> GetColliderBounds(UUID entity) const;
		// Physics.LayerMask(...names) (§11.5): PhysicsLayerTable::MakeMask over the session's layers.
		[[nodiscard]] Result<PhysicsLayerMask> MakeLayerMask(std::span<const std::string_view> names) const;
		[[nodiscard]] const PhysicsLayerTable& GetLayers() const;

		[[nodiscard]] glm::vec3 GetGravity() const;
		// Errors: InvalidArgument for a gravity component that is not finite or above MaxPhysicsGravity in magnitude.
		[[nodiscard]] Status SetGravity(const glm::vec3& gravity);

		// --- Observability (§13.5 physics.bodyInfo, §13.7 stats.get) ------------------------------------------------------
		// The body `entity` owns (its RigidBody, character or implicit static body, before its implicit sensor body when it
		// owns both), else the body its colliders were gathered into; nullopt when neither exists, or when the entity or that
		// body's owner is pending destruction or disabled.
		[[nodiscard]] std::optional<PhysicsBodyReport> GetBodyInfo(UUID entity) const;
		// Every diagnostic the session has raised, each (code, severity, entity, subject) once (a limit's Warning near it and
		// its Error at it are two), sorted by (entity, code, subject, severity). Each
		// is logged once when raised, at Error or Warn by its severity, as "<CODE>: <message>" with the entity's name and id,
		// so "_meta" counts it (§13.4) and tests can expect it (Test::ExpectLog), and then passed to the listener
		// (IPhysicsEventListener::OnPhysicsDiagnostic).
		[[nodiscard]] std::span<const PhysicsDiagnostic> GetDiagnostics() const;
		[[nodiscard]] PhysicsSystemStats GetStats() const;
		// Appends the physics state to the session's state hash (§5.1 "+ physics velocities at runtime", PlaySession::
		// ComputeStateHash): for every created body and character in canonical order of its owner (and origin), the owner's
		// UUID, the body's position, rotation, linear and angular velocity (as the bit patterns of their floats, little
		// endian) and its sleeping flag. A scene without bodies appends nothing, so its hash stays that of M7.
		void AppendStateHash(XXH64Hasher& hasher) const;
	private:
		// The world, the body and character tables, the contact pairs, the events and the diagnostics
		// (Scene/Private/PhysicsSystemState.h, shared by the system's .cpp files).
		struct State;
	private:
		Scope<State> m_State;
	};

	// "CollisionEnter", "CollisionExit", "TriggerEnter" or "TriggerExit".
	[[nodiscard]] std::string_view PhysicsEventTypeToString(PhysicsEventType type);

}
