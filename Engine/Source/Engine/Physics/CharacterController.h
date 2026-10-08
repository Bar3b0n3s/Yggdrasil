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
// ECS-agnostic, like PhysicsWorld; Scene/PhysicsSystem owns one per CharacterControllerComponent and drives it from
// PreStep. Main thread only. Determinism: the same calls in the same order give the same results; the inner body is
// created as the world's next body at Create, so a caller that creates characters and bodies in canonical order (§5.1)
// gets the same inner body handle every run (§9.6: the inner body ID comes from the canonical body-ID sequence).
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 8).

namespace Engine {

	class PhysicsWorld;

	// The fastest a character moves, in m/s: CharacterController::Update clamps the velocity of every step to this length
	// (the terminal speed of a long fall), and Scene/PhysicsSystem::MoveCharacter refuses a desired velocity with a larger
	// component. Far beyond any game's character, and small enough that a step's movement stays well inside Jolt's world
	// bounds whatever the gravity.
	inline constexpr float MaxCharacterSpeed = 500.0f;

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
		// Kilograms in (0, MaxPhysicsMass]: how hard the character pushes dynamic bodies, and the weight it puts on what it
		// stands on (capped per update so no body's velocity can overflow, see Update).
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
	};

	// The ground state after an update.
	struct CharacterGroundState
	{
		bool IsGrounded = false;                              // standing on walkable ground (Jolt's EGroundState::OnGround)
		glm::vec3 GroundNormal = glm::vec3(0.0f, 1.0f, 0.0f); // unit; (0, 1, 0) when not grounded
		glm::vec3 GroundVelocity = glm::vec3(0.0f);           // the velocity of what the character stands on (a moving platform)
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
		// `gravity` * `deltaTime` (the desired vertical part is ignored); that velocity is clamped to MaxCharacterSpeed.
		// "Grounded" is Jolt's OnGround while the character does not move away from the ground faster than 0.1 m/s (else
		// the step after a jump would cancel it). Then ExtendedUpdate with walk-stairs (up to StepHeight) and stick-to-floor
		// (down to StepHeight) over `deltaTime` seconds, which moves the character and its inner body and records its
		// contacts; its body filter ignores the inner body and every body of the character's CollisionGroup. The weight the
		// update puts on the body the character stands on (Mass * |gravity| * deltaTime) is capped at 1e10 N s. An update that
		// would carry the character beyond MaxPhysicsCoordinate leaves it where it was with no velocity (a warning is logged
		// once per controller), so the character always stays where the world places bodies. `deltaTime` > 0 and every
		// vector finite (asserted; Scene/PhysicsSystem validates script input first).
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
		// The ground state of the last Update.
		[[nodiscard]] CharacterGroundState GetGroundState() const;
		// The kinematic inner body (§9.6) that raycasts, overlaps and sensors see.
		[[nodiscard]] BodyHandle GetInnerBody() const;
		[[nodiscard]] uint64_t GetUserData() const;
	private:
		// The CharacterVirtual, its listener and the world back-reference (Physics/CharacterController.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
