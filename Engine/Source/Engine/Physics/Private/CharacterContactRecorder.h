#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/ContactBuffer.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>

#include <vector>

// The character's CharacterContactListener (Architecture §9.6): it records the contacts a CharacterVirtual begins and ends
// while CharacterController::Update runs it, and nothing else. CharacterController turns the records into ContactEvents
// (FromCharacter) in the world's ContactBuffer once the update returned, so the character's entity and the other body
// receive collision events with the same sorting and exit rules as bodies (§9.4); it resolves the other body's collider
// then, with no body locked.
//
// What it records: character-versus-body contacts that Jolt reports through OnContactAdded and OnContactRemoved (one each
// per sub-shape contact: CharacterVirtual tracks the contacts it reported and reports each removal once). Sensors are
// refused in OnContactValidate, so they never reach the character's contact list: triggers detect the character through
// its inner body and the world's ContactListener instead (§9.2, §9.6), and a trigger pair is never reported twice. Contacts
// with other characters do not arise (no CharacterVsCharacterCollision is installed).
//
// Light bodies (M34, Docs/Decisions/0049-m34-characters-input.md decision 2): a Dynamic body lighter than the character
// (IsLighterThanCharacter) never pushes it, so the recorder turns CharacterContactSettings::mCanPushCharacter off for one
// in OnContactAdded and OnContactPersisted; the character still pushes it (mCanReceiveImpulses stays on). Nor does the
// character climb one by sliding up a surface of it that faces upwards: OnContactSolve meets such a contact as a vertical
// wall, so the character's motion into the body stops and its vertical motion stays as it was.
//
// Private to the Physics module. Main thread only: CharacterVirtual calls it synchronously from ExtendedUpdate, in an order
// that depends only on the world's state, so the records are deterministic.

namespace Engine {

	// One contact of a character that began or ended.
	struct CharacterContactRecord
	{
		ContactEventKind Kind = ContactEventKind::Added;
		JPH::BodyID Body{};
		JPH::SubShapeID SubShape{};
		// Added only, world space: the contact point on the body, the normal from the character towards the body (unit) and
		// the relative speed along it (positive when approaching). Finite.
		JPH::RVec3 Point = JPH::RVec3::sZero();
		JPH::Vec3 Normal = -JPH::Vec3::sAxisY();
		float RelativeNormalSpeed = 0.0f;
	};

	// Whether `body` is a Dynamic body lighter than a character of `characterMass` kg (its mass below the character's
	// CharacterControllerDescription::Mass): such a body never moves the character, which pushes it instead, never steps
	// up onto it and puts at most the body's own weight on it (CharacterController.h, M34). A body whose translation is
	// locked has no finite mass and is never light.
	[[nodiscard]] bool IsLighterThanCharacter(const JPH::Body& body, float characterMass);
	// The same for the body `body` read through `locks`; false for a body that no longer exists. Main thread, between
	// steps: the character's update reads through the world's no-lock interface, as CharacterVirtual's own pushes do.
	[[nodiscard]] bool IsLighterThanCharacter(const JPH::BodyLockInterface& locks, const JPH::BodyID& body, float characterMass);

	class CharacterContactRecorder final : public JPH::CharacterContactListener
	{
	public:
		// `locks` (a documented back-reference to the world's no-lock interface, which outlives the recorder) reads the mass
		// of the bodies the character touches.
		explicit CharacterContactRecorder(const JPH::BodyLockInterface& locks);

		CharacterContactRecorder(const CharacterContactRecorder&) = delete;
		CharacterContactRecorder& operator=(const CharacterContactRecorder&) = delete;

		// Refuses sensors (see the file comment).
		bool OnContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override;
		// Records the contact; a light body cannot push the character (see the file comment).
		void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) override;
		// A light body cannot push the character (see the file comment); nothing is recorded.
		void OnContactPersisted(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
			JPH::CharacterContactSettings& settings) override;
		// A light body's upward-facing surface is a vertical wall to the character (see the file comment).
		void OnContactSolve(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID& subShape,
			JPH::RVec3Arg contactPosition, JPH::Vec3Arg contactNormal, JPH::Vec3Arg contactVelocity, const JPH::PhysicsMaterial* contactMaterial,
			JPH::Vec3Arg characterVelocity, JPH::Vec3& newCharacterVelocity) override;
		void OnContactRemoved(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID& subShape) override;

		// The records since the last Drain, in callback order, and empties the list.
		[[nodiscard]] std::vector<CharacterContactRecord> Drain();
	private:
		// Turns mCanPushCharacter off for a light body.
		void ApplyLightBodyRule(const JPH::CharacterVirtual& character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) const;
	private:
		const JPH::BodyLockInterface* m_Locks = nullptr;
		std::vector<CharacterContactRecord> m_Records;
	};

}
