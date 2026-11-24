#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsJoint.h"
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
//          changed; a collider's mesh got a new asset version (a hot reload, §7.5); a TerrainCollider's Terrain changed or
//          its Heightfield got a new asset version (M26: terrain.edit, a reimport; Docs/Decisions/0037-m26-contract.md
//          decision 21); an entity became effectively enabled; or a gathered collider moved relative to its body. A
//          rebuild caused by colliders alone (their components, the hierarchy, scales, meshes, moves) that leaves the
//          body's motion type and sensor flag as they were replaces its shape in place (PhysicsWorld::SetShape: handle,
//          pose, velocities and Jolt's contact bookkeeping kept); any other rebuild (a RigidBody or CharacterController
//          change) destroys the body and creates it again with its current linear and angular velocity. The RigidBody's
//          Initial* velocities apply only to a Dynamic body created for an owner that had none (session start, a new
//          component or entity, a re-enabled entity); a Kinematic body follows its Transform. An entity that became
//          disabled or is pending destruction keeps the body it has until FlushDestroyed (step 8), which closes its
//          pairs, and gets none created; PreStep never removes it.
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
//     M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 3): between items 1-2 and 3, the collider surfaces. A
//     ColliderSurface's signals mark its entity (never ComponentsChanged, so a surface write never recomposes the scene), and
//     PreStep sends the surfaces of every body holding a marked collider (PhysicsWorld::SetColliderSurfaces, the velocity
//     turned into the body's axes); a body created or whose shape is replaced gets its surfaces at once. A write in the fixed
//     step is in force at that tick's PreStep, one in the frame phase at the next tick's (§9.2).
//   - Step (step 6): PhysicsWorld::Step(fixedDelta, max(1, ceil(60 / FixedHz))).
//   - PostStep (step 7): Dynamic bodies that are active or have a parent write their world pose back as their entity's
//     local Transform (through the parent's current inverse, so the entity stays where the body is even when the parent
//     moved), and characters theirs, visiting bodies in canonical order, never in Jolt's active-body order; then the
//     contacts are drained, mapped to owners, collapsed into body-pair begin and end events with a per-pair reference
//     count of sub-shape contacts (net over the step's records: a pair whose count rises from zero enters, once it holds a
//     touching contact (the M20 paragraph), one whose count falls to zero exits), sorted by PhysicsEventOrderKey, and
//     dispatched (§9.4 steps 1 to 4, PhysicsEvent, IPhysicsEventListener). Sleeping never ends a pair: a Removed record
//     for a contact whose bodies fell asleep leaves it dormant (it still counts), and a dormant contact is dropped only
//     after a step that began with one of its bodies awake and did not report it again.
//   - FlushDestroyed (step 8, and the frame phase's destroy flush, before Scene::FlushPendingDestroys and after the
//     scripts' OnDestroy): every active pair involving an entity marked for destruction (PendingDestroyTag) or effectively
//     disabled is closed, each surviving side that received the pair's enter getting a synthesized exit (§9.4 "Exit on
//     destroy, disable and character contacts"), dispatched in sorted order; then those entities' bodies and characters
//     are removed (a re-enabled entity gets its body back at the next PreStep or SynchronizeBodies). The listener may
//     destroy or disable more entities during that dispatch, so the flush repeats its close, dispatch and remove pass
//     until a pass finds nothing left to do; each entity is handled once, which bounds the passes by the entity count
//     (M13's OnDestroy for entities destroyed there joins the same loop). When it returns, no entity pending destruction
//     or disabled has a body, a character or an active pair.
//
// M20 (Docs/Decisions/0031-m20-contract.md decisions 2 and 3):
//   - The pending enter (W-P03). A contact that Jolt added without touching (ContactEvent::Touching false: a speculative
//     contact, Physics/ContactBuffer.h) counts for its pair's lifetime but cannot start it: a pair enters at the first
//     PostStep after which it holds a touching contact (an Added record that touches, or a Touched record), its enter's
//     contact being the first of its touching contacts of that step by the same total order as today. A pair whose
//     contacts all end before one touched reports nothing, neither enter nor exit; an entered pair exits when its last
//     contact, touching or not, ends. The rule runs inside PostStep; it adds no PlaySessionPhase.
//   - Collider events (W-P01). A scripted gathered collider, an entity whose Script component names a script (Script
//     version 1: a valid Script handle; version 2: a slot with a script, decision 17) and whose solid colliders are
//     gathered into another owner's body (a RigidBody's compound or an implicit static body's tree; it is in the body's
//     PhysicsBodyEntry::Colliders and is not the body's owner), receives events of its own. The system reads the component
//     alone, never the script or a VM, so a Simulate session decides the same way. A collider pair, keyed by
//     (the collider entity, the partner's body owner, kind), counts only the contacts of the collider's own sub-shapes and
//     follows every body-pair rule (the pending enter, dormant contacts, rebuilds and unconfirmed pairs, the drop rules,
//     synthesized exits at the destroy and disable flushes of either side) with the collider entity as its only receiving
//     side: PhysicsEvent::Self is the collider entity, SelfBody its body's owner, Other the partner's body owner, and the
//     contact names the collider itself and the partner's collider of the first sub-shape pair, its normal pointing from
//     the collider's body towards the partner. The partner and the body's owner keep their body-pair events unchanged. A
//     collider that stops being a scripted gathered collider (its Script component removed or naming no script any more, a
//     RigidBody of its own, moved out of the body) ends its collider pairs at the next PostStep with synthesized exits, and
//     one that becomes one while touching enters at the next PostStep. A collider pair is a collision or a trigger pair as
//     its body pair is, and it counts the sub-shape contacts Jolt reports: Jolt's manifold reduction merges a body pair's
//     contacts whose normals agree into the manifold of the first sub-shape pair it found, so of two coplanar pieces under
//     one body, only that one counts the merged contact.
//   - Order. Pair events are sorted by PhysicsEventOrderKey (Low owner, High owner, type, receiving collider); a body
//     event's key has the null collider, so body events keep their relative order and a scene without scripted gathered
//     colliders dispatches exactly as before. M21's 2D world and M26's height fields dispatch with the same key.
//
// M37 (Docs/Decisions/0054-m37-joints.md; Architecture §9.10; the work in Scene/PhysicsSystemJoints.cpp): joints.
//   - Synchronization. After the bodies of every rebuild, and in PreStep and SynchronizeBodies whenever a joint component
//     changed or a body was created or removed, the joints are brought in step with ComposePhysicsJoints (Scene/
//     PhysicsJoints.h) in canonical order of their entities: a joint whose plan is creatable, whose component is not Broken
//     and whose two bodies exist is created (PhysicsWorld::CreateJoint) or, when it lives already, has its settings applied
//     in place (PhysicsWorld::SetJointSettings, waking its bodies); any other is not in force. A joint whose entity or end
//     is leaving play (pending destruction or disabled) keeps what it has until FlushDestroyed, and a reason its plan has
//     then is raised after the flush, which composes the plans again. The plan is composed again
//     only when the bodies were rebuilt, a joint component was added or removed, or a changed joint's type, ConnectedBody,
//     Anchor, Axis, ConnectedAnchor or limits differ from what the last plan read, so a script writing a motor target every
//     tick changes the live joint and composes nothing. Its PHYSICS_JOINT_INVALID diagnostics are raised as the body
//     composition's are. Every joint in force gets its entity's rank in UUID order as its solver priority, refreshed
//     whenever the joints in force change.
//   - Kept frames. A hinge's, slider's or fixed joint's two frames are formed when it is first created, from the poses of
//     that moment (PhysicsJoints.h's MakePhysicsJointWorldFrame, expressed in each body's space relative to its origin),
//     and kept once the creation succeeds: a body created again (a RigidBody change, a re-enabled entity, a refusal undone) gets its joints back at
//     the same frames, so a door that swung keeps its zero and its limits. They are formed again when the joint's type,
//     ConnectedBody, Anchor or Axis, the bodies its ends resolve to, or the entities between its entity and its body
//     change. A distance joint keeps nothing: its two anchors are read from the scene at every creation, and it is made
//     again when the same fields or its ConnectedAnchor change. Frames are dropped with the joint component or its entity.
//   - Bodies first. RemoveBody destroys the body's joints before the body (the world would too), on every removal path: a
//     RigidBody change, a refusal, the disable and destroy flushes and the session's end.
//   - Breaking. At the start of PostStep, every joint in force with a BreakForce or BreakTorque compares its force and
//     torque of the last collision step (PhysicsWorld::GetJointState) with them, in UUID order; one over either is removed
//     (its bodies woken), Broken is written into its component (scene data, so hashed), and its PhysicsJointBreak is
//     dispatched (IPhysicsEventListener::OnPhysicsJointBreak) after that PostStep's pair events. A Broken joint is never
//     created; Broken written false mends it at the next synchronization, at its kept frames.
//   - FlushDestroyed removes the joints of entities that left play: a destroyed entity's joint and frames go; a disabled
//     one's joint goes and its frames stay for its return.
//
// Determinism (§9.1): bodies, characters, write-back, events and the state hash all follow canonical order or UUID order;
// nothing iterates an unordered container to produce output; results do not depend on the worker thread count.
//
// Threading: main thread only (§4.11). Not copyable or movable. The system reads and writes its scene directly (a runtime
// system, §5.1), so its writes do not count in the scene's revision.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decisions 11 to 13); M20, which owns the contact path in
// phase 1, extends it (the M20 paragraph above; Docs/Decisions/0031-m20-contract.md); M36 adds the collider surfaces (the
// step's M36 paragraph), PhysicsCharacterState's GroundCollider, IsSqueezed and SqueezedBy, PhysicsSurfaceInfo and GetSurface
// (Docs/Decisions/0053-m36-contact-surfaces.md decisions 3 to 5 and 12), and M37 appends its joints (the M37 paragraph
// above), each reviewed as a change to a frozen interface.

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
		// The session's asset manager, outliving the system: MeshCollider meshes and (M26) the heightfields of
		// TerrainColliders' Terrains. Null makes every mesh and terrain collider a PHYSICS_INVALID_SHAPE diagnostic.
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
		// M20 (Docs/Decisions/0031-m20-contract.md decision 2): the owner of the body the event concerns on Self's side:
		// for a collider event, whose Self is the scripted gathered collider (see the file comment), that body's owner; for a
		// body event Self, or the null UUID (the default), which also means a body event, so an event built without it
		// (a test's, or a later milestone's dispatch) is a body event. The system sets it to Self on every body event.
		UUID SelfBody{};

		// Whether this is a collider event: SelfBody is set and differs from Self.
		[[nodiscard]] bool IsColliderEvent() const { return SelfBody.IsValid() && SelfBody != Self; }

		bool operator==(const PhysicsEvent&) const = default;
	};

	// The canonical order of pair events (§9.4 step 2, Docs/Decisions/0031-m20-contract.md decision 2): every system that
	// dispatches PhysicsEvents sorts its pair events by this key, ascending, compared member by member (UUIDs by value).
	// Low and High are the pair's two body owners, the lower UUID first; Collider is the null UUID for a body event and the
	// receiving collider entity for a collider event. A body event delivers its callbacks to Low, then High; a collider
	// event its one callback to Collider.
	struct PhysicsEventOrderKey
	{
		UUID Low{};
		UUID High{};
		PhysicsEventType Type = PhysicsEventType::CollisionEnter;
		UUID Collider{};

		auto operator<=>(const PhysicsEventOrderKey&) const = default;
	};

	// The key of the pair event `event` was delivered for: Low and High from Self's body owner (SelfBody, or Self when
	// SelfBody is null) and Other, Collider = Self for a collider event and the null UUID otherwise. Both callbacks of a body
	// event map to one key, whether or not they set SelfBody.
	[[nodiscard]] PhysicsEventOrderKey GetPhysicsEventOrderKey(const PhysicsEvent& event);

	// M37 (Docs/Decisions/0054-m37-joints.md decision 6): a joint that broke at a PostStep: its force or torque over the last
	// collision step passed its BreakForce or BreakTorque, so the system removed it and wrote Broken into its component.
	struct PhysicsJointBreak
	{
		UUID Joint{};        // the joint's entity
		float Force = 0.0f;  // N, as measured (PhysicsJointState::Force)
		float Torque = 0.0f; // N m
		uint64_t Tick = 0;   // the tick whose PostStep broke it

		bool operator==(const PhysicsJointBreak&) const = default;
	};

	// M37 (Docs/Decisions/0054-m37-joints.md decision 7): what scripts and physics.jointInfo read of a joint
	// (PhysicsSystem::GetJointInfo).
	struct PhysicsJointReport
	{
		UUID Entity{};
		PhysicsJointType Type = PhysicsJointType::Hinge;
		// The owners of the joint's body and of the connected body, as the joint composition resolved them (invalid when an
		// end has none; Connected is also invalid for the world).
		UUID Body{};
		UUID Connected{};
		// Whether the joint is in force: false for a refused, broken, disabled or not yet created joint.
		bool Created = false;
		// The component's Broken.
		bool Broken = false;
		// While Created, the reading after the last step (and a teleport since): a hinge's angle in degrees, a slider's position
		// in metres, a distance joint's distance in metres; the force (N) and torque (N m) of the last collision step, as the
		// break test measures them. Zero otherwise, and the readings of the other types are zero.
		float Angle = 0.0f;
		float Position = 0.0f;
		float Distance = 0.0f;
		float Force = 0.0f;
		float Torque = 0.0f;
		// A hinge's or slider's motor (Off for the other types).
		PhysicsJointMotor Motor = PhysicsJointMotor::Off;
		// While Created, the joint in world space as the bodies hold it (PhysicsJointState::Point, ConnectedPoint and Axis): its
		// point on its body and on the connected body, and a hinge's or slider's axis; zero otherwise. The debug view draws them
		// in play.
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 ConnectedPoint = glm::vec3(0.0f);
		glm::vec3 Axis = glm::vec3(0.0f);
	};

	// Receives the sorted events and the run-time diagnostics (the interface scripting consumes in M13: the ScriptEngine
	// dispatches each event to the scripts on Self, and turns diagnostics about refused bodies into script errors, §9.1).
	// Implementations live outside Scene; tests record them.
	class IPhysicsEventListener
	{
	public:
		virtual ~IPhysicsEventListener() = default;

		// Called on the main thread from PostStep and FlushDestroyed, once per event, in this order: pair events sorted by
		// PhysicsEventOrderKey, and for each body event first the callback whose Self is the lower UUID, then the other (a
		// collider event has one callback, to its collider). What is dropped (§9.4 step 3):
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

		// M37 (Docs/Decisions/0054-m37-joints.md decision 6): called on the main thread from PostStep, after that PostStep's pair
		// events, once per joint that broke there (PhysicsJointBreak), in UUID order of the joints' entities; the rules of
		// OnPhysicsEvent apply (a break whose joint entity an earlier callback destroyed or disabled is not delivered). The
		// default ignores them.
		virtual void OnPhysicsJointBreak(const PhysicsJointBreak& /*jointBreak*/) {}
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

	// A character's state (§9.6 CharacterController:IsGrounded, GetGroundNormal, GetVelocity; M36's GetGroundCollider).
	struct PhysicsCharacterState
	{
		bool IsGrounded = false;
		glm::vec3 GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f);
		glm::vec3 Velocity = glm::vec3(0.0f);
		// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 4): the collider entity the last update stood on (another
		// character's entity when it stood on one); the null UUID when it was not grounded, or when that entity or its body's
		// owner is disabled or pending destruction now.
		UUID GroundCollider{};
		// M36 W36.2 (Docs/Decisions/0053-m36-contact-surfaces.md decision 12): whether the last update found the character
		// squeezed (CharacterController::GetSqueeze), and the owner of the squeezing body; SqueezedBy is the null UUID when it
		// is not squeezed, or when that owner is disabled or pending destruction now.
		bool IsSqueezed = false;
		UUID SqueezedBy{};
	};

	// M47 (Docs/Decisions/0065-m47-water.md decision 3; physics.bodyInfo's buoyancy): what the last PreStep's buoyancy item
	// (4c) found for a body whose owner holds a Buoyancy: the water its planes came from (the Buoyancy's Water when set, else
	// the water that held the most of the body's submerged volume; the null UUID when nothing was submerged) and the body's
	// submerged and total volumes (m^3; 0 before the first tick it was applied, and while it is skipped).
	struct PhysicsBuoyancyReport
	{
		UUID Water{};
		float SubmergedVolume = 0.0f;
		float TotalVolume = 0.0f;
	};

	// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 5; §11.5 Physics.GetSurface's SurfaceInfo): what contacts
	// with a collider use.
	struct PhysicsSurfaceInfo
	{
		float Friction = 0.5f;
		float Restitution = 0.0f;
		glm::vec3 SurfaceVelocity = glm::vec3(0.0f); // world space, m/s
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
		// M47 (decision 3): bodies whose owner holds a Buoyancy only (empty otherwise); filled by W47.2's physics stream.
		std::optional<PhysicsBuoyancyReport> Buoyancy{};
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
		// PreStep's rebuilds (its item 1, with item 2's pairs across rebuilds and the partners it wakes) alone, without its
		// kinematic moves, teleports and character updates, when a component change marked an owner since the last PreStep
		// or synchronization; nothing otherwise. The play session runs it before every round of a start flush (§5.7 step 2,
		// and the startup flushes of the session and of Scene.Load and Scene.Reload; ADR 0038 decision 14), after a
		// TransformSystem::Update, so an entity has its body from its OnStart on, wherever it was created, and every physics
		// component or Active change made after a tick's PreStep is in force at the next tick's OnFixedUpdate. Diagnostics are
		// heard by the listener as PreStep's are. Not from a listener callback (asserted).
		void SynchronizeBodies();
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
		// M36 (Docs/Decisions/0053-m36-contact-surfaces.md decision 5; §11.5 Physics.GetSurface): the values contacts with
		// `entity`'s solid colliders use: its ColliderSurface's Friction and Restitution and its SurfaceVelocity turned into
		// world space by the entity's world rotation, as the component holds them now (the world takes a write at the next
		// PreStep), or else its body's friction and restitution with no velocity; nullopt when the entity has no solid
		// collider in a created body (a character's capsule, triggers alone, 2D colliders) or is pending destruction or
		// disabled.
		[[nodiscard]] std::optional<PhysicsSurfaceInfo> GetSurface(UUID entity) const;
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
		// endian) and its sleeping flag. A scene without bodies appends nothing, so its hash stays that of M7. M37 (Docs/
		// Decisions/0054-m37-joints.md decision 5): then the joint part, for every joint whose frames the session keeps, in
		// canonical order of its entity: the entity's UUID and the bits of its two kept frames (point, axis and normal on the
		// joint's body, then on the connected body); nothing without one, so a scene without joints hashes as before.
		void AppendStateHash(XXH64Hasher& hasher) const;

		// --- Joints (M37, Docs/Decisions/0054-m37-joints.md; Scene/PhysicsSystemJoints.cpp) ---------------------------------
		// The joint of `entity` (its HingeJoint, SliderJoint, DistanceJoint or FixedJoint) as scripts (HingeJoint:GetAngle and
		// the others) and physics.jointInfo read it; nullopt when the entity does not exist or holds no joint component.
		[[nodiscard]] std::optional<PhysicsJointReport> GetJointInfo(UUID entity) const;
		// The joints that broke at the last PostStep, in UUID order of their entities (cleared at the start of each PostStep;
		// what tests and observability read).
		[[nodiscard]] std::span<const PhysicsJointBreak> GetLastJointBreaks() const;
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
