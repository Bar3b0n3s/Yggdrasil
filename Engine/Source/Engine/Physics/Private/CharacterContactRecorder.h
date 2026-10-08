#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/ContactBuffer.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/BodyID.h>
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

	class CharacterContactRecorder final : public JPH::CharacterContactListener
	{
	public:
		// Refuses sensors (see the file comment).
		bool OnContactValidate(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact) override;
		void OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact, JPH::CharacterContactSettings& settings) override;
		void OnContactRemoved(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID& subShape) override;

		// The records since the last Drain, in callback order, and empties the list.
		[[nodiscard]] std::vector<CharacterContactRecord> Drain();
	private:
		std::vector<CharacterContactRecord> m_Records;
	};

}
