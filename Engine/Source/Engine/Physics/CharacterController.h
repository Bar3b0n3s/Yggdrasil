#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstdint>

// The character controller (Architecture §9.6): a JPH::CharacterVirtual moved with ExtendedUpdate (stair stepping, floor
// sticking). A CharacterVirtual is not in the broad phase, so the controller creates an inner body, a kinematic capsule of
// the controller's size on the controller's layer, as the world's next body (PhysicsWorld::CreateBody, so the world's
// bookkeeping, limits and collision groups cover it), and places it at the character after every update and teleport:
// raycasts, overlaps and sensors see the character through that inner body (§9.2: sensors set
// mCollideKinematicVsNonDynamic). A CharacterContactListener records the character's own contacts into
// the world's ContactBuffer (ContactEvent::FromCharacter, BodyA the inner body), so the character's entity and the other
// body receive collision events with the same sorting and exit rules as bodies (§9.4).
//
// Characters and Dynamic bodies (M34, Docs/Decisions/0049-m34-characters-input.md decisions 1 to 3, World Tree's brief
// 11). The inner body's object layer has the Character kind (PhysicsLayers.h), so the solver never resolves a contact
// between it and another body but a sensor: placed rather than swept, it would push a Dynamic body with infinite mass. A
// character pushes Dynamic bodies only through CharacterVirtual's own contacts, with at most Mass * 9.81 N, and a Dynamic
// body lighter than its Mass never moves it: it cannot push the character or carry it up a surface of its own (but the
// one the character stands on, which it walks across), gives it none of its velocity as its ground (so standing on one
// never launches it), is never a stair (the character pushes it along instead of climbing it) and carries at most its own
// weight from the character. Characters still block each other through their inner bodies. What that leaves (§9.6,
// Docs/Backlog.md §6): a light body moving into a character is stopped by the character's push, and passes into the
// capsule only when it outruns the character's look-ahead within one tick or is driven harder than the push; a light
// platform never lifts or carries a character; a heavy body shoves the character, and overlaps it only when it is pinned
// or under the body.
//
// ECS-agnostic, like PhysicsWorld; Scene/PhysicsSystem owns one per CharacterControllerComponent and drives it from
// PreStep. Main thread only. Determinism: the same calls in the same order give the same results; the inner body is
// created as the world's next body at Create, so a caller that creates characters and bodies in canonical order (§5.1)
// gets the same inner body handle every run (§9.6: the inner body ID comes from the canonical body-ID sequence).
//
// Surfaces (M36, Docs/Decisions/0053-m36-contact-surfaces.md decision 4; World Tree's briefs 15 and 16). The ground state
// names the body and the collider the character stands on (Jolt's ground body and sub-shape of the update, no query), and a
// collider surface's velocity (PhysicsWorld::SetColliderSurfaces) carries a grounded character: a conveyor belt moves it
// whatever InheritGroundVelocity says, which stays about ground that really moves.
//
// Pushes and the squeeze (M36 W36.2, ADR 0053 decisions 11 and 12; World Tree's brief 16). A Kinematic body, or a Dynamic
// body not lighter than the character, that moves into the character pushes it along the contact's normal at the body's
// velocity, through CharacterVirtual's moving contact planes: while the body is within the character's look-ahead (0.1 m)
// at its update, so up to about 6 m/s at 60 Hz with no overlap; a faster one overlaps the character for one tick and pushes
// it out at the next update, and one beyond about 0.1 m plus Radius a tick can pass through it (Docs/Backlog.md §6). A
// character such a body cannot move out of the way (pinned under a press, against a wall by a piston) is squeezed: after its
// update, the body's push this step would leave it overlapping the character by more than CharacterSqueezeTolerance
// (GetSqueeze). The engine makes no escape and deals no damage: the game decides.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 8); M34 changes what it documents of Mass,
// Update and the inner body, reviewed as a change to a frozen interface (ADR 0049 decision 1); M36 adds the ground's body,
// collider and surface velocity and the squeeze, reviewed as such a change too (ADR 0053 decisions 4 and 12).

namespace Engine {

	class PhysicsWorld;

	// The fastest a character moves, in m/s: CharacterController::Update clamps the velocity of every step to this length
	// (the terminal speed of a long fall), and Scene/PhysicsSystem::MoveCharacter refuses a desired velocity with a larger
	// component. Far beyond any game's character, and small enough that a step's movement stays well inside Jolt's world
	// bounds whatever the gravity.
	inline constexpr float MaxCharacterSpeed = 500.0f;

	// M36 W36.2 (Docs/Decisions/0053-m36-contact-surfaces.md decision 12): how far, in metres, a body that pushes a character
	// may end its step inside the character's padded capsule before the character counts as squeezed (GetSqueeze). Below one
	// tick of a press at 1 m/s (1.6 cm at 60 Hz), above what Jolt's solver leaves of a push it follows (its tolerances are a
	// millimetre).
	inline constexpr float CharacterSqueezeTolerance = 0.01f;

	// What a controller is created with (CharacterControllerComponent, §5.3). Every float finite.
	struct CharacterControllerDescription
	{
		// The capsule: total height and radius in metres, Radius > 0 and Height > 2 * Radius (a capsule with a cylinder),
		// else PHYSICS_INVALID_SHAPE.
		float Height = 1.8f;
		float Radius = 0.3f;
		// The steepest walkable slope, degrees in [0, 90].
		float MaxSlopeAngle = 45.0f;
		// The highest step climbed without jumping (ExtendedUpdate's walk-stairs step up), metres >= 0.
		float StepHeight = 0.3f;
		// Kilograms in (0, MaxPhysicsMass]: how hard the character pushes Dynamic bodies (at most Mass * 9.81 N, M34), which
		// Dynamic bodies are light for it (a mass below Mass: they never move the character, see the file comment), and the
		// weight it puts on a Dynamic body it stands on (at most that body's own weight, and capped per update so no body's
		// velocity can overflow, see Update).
		float Mass = 70.0f;
		// The project layer of the character and its inner body (< the world's layer count).
		uint32_t Layer = 0;
		// The character's base (the bottom of its capsule) and its rotation (unit; only the rotation about the up axis is
		// meaningful).
		PhysicsPose Pose{};
		// The owner's data, also the inner body's user data (PhysicsWorld::GetUserData): Scene/PhysicsSystem stores the
		// entity's UUID.
		uint64_t UserData = 0;
		// The inner body's collision group (BodyDescription::CollisionGroup), which the character's own collision honours
		// too: Update ignores every body of the same non-zero group. Scene/PhysicsSystem passes the owner's UUID, so the
		// trigger colliders attached to the character (a pickup radius, §5.3) never report it.
		uint64_t CollisionGroup = 0;
		// M19 (Docs/Decisions/0030-m19-contract.md decision 36, P-CARRY; CharacterControllerComponent::InheritGroundVelocity):
		// while grounded, the character also moves with the horizontal velocity of what it stands on (see Update), so a
		// platform moving at v carries it v * t in t seconds. Off by default: the character then keeps only the ground's
		// vertical velocity, as before.
		bool InheritGroundVelocity = false;
	};

	// The ground state after an update.
	struct CharacterGroundState
	{
		bool IsGrounded = false;                              // standing on walkable ground (Jolt's EGroundState::OnGround)
		glm::vec3 GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f); // unit; (0, 1, 0) when not grounded
		// The velocity of what the character stands on (a moving platform), M36: with the surface velocity of the collider it
		// stands on (PhysicsWorld::SetColliderSurfaces, in world space at the update), which a Dynamic body lighter than the
		// character never gives.
		glm::vec3 GroundVelocity = glm::vec3(0.0f);
		// M36: the body the character stands on (Jolt's ground body of the update: another character's inner body when it stands
		// on one) and the collider of it (ColliderShapeDescription::UserData, as a contact on that sub-shape names it); the
		// invalid handle and 0 when not grounded. The body may have been destroyed since the update.
		BodyHandle GroundBody{};
		uint32_t GroundCollider = 0;
	};

	// M36 W36.2: whether the last update found the character squeezed, and by which body (GetSqueeze).
	struct CharacterSqueeze
	{
		bool IsSqueezed = false;
		// The squeezing body of the lowest handle (canonical creation order) when several squeeze it; invalid otherwise. It
		// may have been destroyed since the update.
		BodyHandle Body{};
	};

	class CharacterController
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class CharacterController;
		};

		// Use Create.
		explicit CharacterController(ConstructionKey key);
		// Removes the inner body from the world.
		~CharacterController();

		CharacterController(const CharacterController&) = delete;
		CharacterController& operator=(const CharacterController&) = delete;

		// A controller in `world` (a documented back-reference: the world must outlive the controller) with its inner body,
		// and the CharacterContactListener over the world's ContactBuffer. Errors: InvalidArgument for a non-finite value,
		// a value outside its range or a layer outside the world's table; Validation "PHYSICS_INVALID_SHAPE: ..." for a
		// capsule without a cylinder (Height <= 2 * Radius) or one Jolt refuses; InvalidState "PHYSICS_LIMIT_EXCEEDED: ..."
		// when the world cannot hold another body.
		[[nodiscard]] static Result<Scope<CharacterController>> Create(PhysicsWorld& world, const CharacterControllerDescription& description);

		// One step of the character (§9.6, run in PreStep before the world steps): the velocity for the step is the
		// horizontal part of `desiredVelocity` plus a vertical part, which is, while grounded, the ground's vertical velocity
		// plus the desired vertical part (a jump when positive) and, in the air, the previous vertical velocity plus
		// `gravity` * `deltaTime` (the desired vertical part is ignored); M19 (Docs/Decisions/0030-m19-contract.md decision
		// 36): with CharacterControllerDescription::InheritGroundVelocity and while grounded, plus the horizontal part of the
		// ground's velocity (Jolt's ground velocity, updated first: a kinematic platform's velocity, a rotating body's surface
		// velocity at the contact); M36 (ADR 0053 decision 4): the ground's velocity includes the surface velocity of the collider
		// the character stands on (turned by its body's current rotation; none on a light body), and while grounded its
		// horizontal part is added whatever InheritGroundVelocity says, so a conveyor belt carries the character; that velocity
		// is clamped to MaxCharacterSpeed.
		// "Grounded" is Jolt's OnGround while the character does not move away from the ground faster than 0.1 m/s (else
		// the step after a jump would cancel it); the ground's velocity is none when the ground is a Dynamic body lighter than
		// the character (M34). Then ExtendedUpdate with stick-to-floor (down to StepHeight) and ExtendedUpdate's walk-stairs
		// step (up to StepHeight), never onto a Dynamic body lighter than the character, over `deltaTime` seconds, which
		// moves the character and its inner body and records its contacts; its body filter ignores the inner body and every
		// body of the character's CollisionGroup. The weight the update puts on a Dynamic body the character stands on
		// (Mass times the gravity's part along the ground's normal times `deltaTime`, at the contact; at most that body's own
		// weight when it is lighter than the character) is capped at 1e10 N s. An update that would carry the character
		// beyond MaxPhysicsCoordinate leaves it where it was with no velocity (a warning is logged once per controller), so
		// the character always stays where the world places bodies. `deltaTime` > 0 and every vector finite (asserted;
		// Scene/PhysicsSystem validates script input first).
		void Update(float deltaTime, const glm::vec3& desiredVelocity, const glm::vec3& gravity);

		// The character's base position and rotation.
		[[nodiscard]] PhysicsPose GetPose() const;
		// Teleports the character and its inner body; velocity and ground state are kept until the next Update. `pose`
		// satisfies IsPlaceablePhysicsPose (asserted; the call is ignored otherwise).
		void SetPose(const PhysicsPose& pose);
		// The velocity of the last Update (world space, m/s).
		[[nodiscard]] glm::vec3 GetVelocity() const;
		// Sets the velocity the next Update starts from (Scene/PhysicsSystem uses it for teleports that zero it).
		void SetVelocity(const glm::vec3& velocity);
		// The ground state of the last Update (its ground's surface velocity as that update turned it).
		[[nodiscard]] CharacterGroundState GetGroundState() const;
		// M36 W36.2: the squeeze of the last Update (see the file comment). A body pushes the character when it is Kinematic, or
		// Dynamic and not lighter than the character, and its velocity at a contact of the update moves it towards the character
		// faster than 1 mm/s; it squeezes the character when, at the character's position after the update, its push over the
		// step (that speed times the step) would leave it more than CharacterSqueezeTolerance inside the character's padded
		// capsule: the update could not move the character ahead of it, so it is held by something else (the floor under a
		// press, a wall behind a piston). A body the character merely touches, one at rest (a resting ram the character is carried
		// into), one moving away, a static wall the character walks into, a corner or a steep face never squeezes; a press's first
		// contact, while it is still above the character's head, does not either. Read with one extra shape query of the
		// character's capsule against the pushing bodies alone, only in an update that met one. A teleport keeps the last
		// update's value.
		[[nodiscard]] CharacterSqueeze GetSqueeze() const;
		// The kinematic inner body (§9.6) that raycasts, overlaps and sensors see; its object layer has the Character kind
		// (M34), so no other body meets it in the solver.
		[[nodiscard]] BodyHandle GetInnerBody() const;
		[[nodiscard]] uint64_t GetUserData() const;
		// Sets CharacterControllerDescription::InheritGroundVelocity in place, for the next Update on: a per-update switch, so
		// the character, its ground state and its inner body stay (Docs/Decisions/0030-m19-contract.md decision 36).
		void SetInheritGroundVelocity(bool inherit);
	private:
		// The CharacterVirtual, its listener and the world back-reference (Physics/CharacterController.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
