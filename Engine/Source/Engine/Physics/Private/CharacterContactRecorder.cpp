#include "EnginePCH.h"
#include "Engine/Physics/Private/CharacterContactRecorder.h"

#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/MotionProperties.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsFinite(JPH::Vec3Arg value)
		{
			return std::isfinite(value.GetX()) && std::isfinite(value.GetY()) && std::isfinite(value.GetZ());
		}

		// The length below which a contact normal's horizontal part has no direction: the surface faces straight up or down.
		constexpr float MinWallNormalLength = 1.0e-3f;

		// Whether `character`'s last update stood on `body`: its walkable ground (Jolt's OnGround and ground body), which both
		// light-body rules except.
		[[nodiscard]] static bool StandsOn(const JPH::CharacterVirtual& character, const JPH::BodyID& body)
		{
			return character.GetGroundState() == JPH::CharacterBase::EGroundState::OnGround && character.GetGroundBodyID() == body;
		}

	}

	bool IsLighterThanCharacter(const JPH::Body& body, float characterMass)
	{
		if (!body.IsDynamic())
			return false;
		// mass < characterMass, without dividing: Jolt keeps the inverse mass (0 when translation is locked).
		return body.GetMotionProperties()->GetInverseMass() * characterMass > 1.0f;
	}

	bool IsLighterThanCharacter(const JPH::BodyLockInterface& locks, const JPH::BodyID& body, float characterMass)
	{
		const JPH::BodyLockRead lock(locks, body);
		return lock.SucceededAndIsInBroadPhase() && IsLighterThanCharacter(lock.GetBody(), characterMass);
	}

	CharacterContactRecorder::CharacterContactRecorder(const JPH::BodyLockInterface& locks)
		: m_Locks(&locks)
	{
	}

	bool CharacterContactRecorder::OnContactValidate(const JPH::CharacterVirtual* /*character*/, const JPH::CharacterContact& contact)
	{
		return !contact.mIsSensorB;
	}

	void CharacterContactRecorder::ApplyLightBodyRule(const JPH::CharacterVirtual& character, const JPH::CharacterContact& contact,
		JPH::CharacterContactSettings& settings) const
	{
		if (contact.mMotionTypeB != JPH::EMotionType::Dynamic || !IsLighterThanCharacter(*m_Locks, contact.mBodyB, character.GetMass()))
			return;
		// M48: the walkable surface of the light body the character stands on supports it; its steep faces, and every other
		// light body, never push it (see the file comment).
		settings.mCanPushCharacter = Utils::StandsOn(character, contact.mBodyB) && !character.IsSlopeTooSteep(contact.mContactNormal);
	}

	void CharacterContactRecorder::OnContactPersisted(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
		JPH::CharacterContactSettings& settings)
	{
		ApplyLightBodyRule(*character, contact, settings);
	}

	void CharacterContactRecorder::OnContactSolve(const JPH::CharacterVirtual* character, const JPH::BodyID& body, const JPH::SubShapeID& /*subShape*/,
		JPH::RVec3Arg /*contactPosition*/, JPH::Vec3Arg contactNormal, JPH::Vec3Arg /*contactVelocity*/, const JPH::PhysicsMaterial* /*contactMaterial*/,
		JPH::Vec3Arg characterVelocity, JPH::Vec3& newCharacterVelocity)
	{
		// Jolt slides the character along the contact's plane, which for a surface facing upwards (a shard's sloped top, a
		// box's edge below the capsule's middle) carries it up onto the body. A surface facing downwards or straight up keeps
		// Jolt's answer, which never lifts the character, and so does the body the character stands on (its ground of the
		// last update, walkable): the rule stops a character getting onto a light body, never moving across the one it is on,
		// such as up a tilted plank. M48 (decision 4): nor does it stop a character standing on a light body walking onto
		// another light body's walkable face, as onto any ground (the next plank of a bridge, where the deck climbs).
		const JPH::Vec3 up = character->GetUp();
		const float upwards = contactNormal.Dot(up);
		JPH::Vec3 across = contactNormal - upwards * up;
		const float length = across.Length();
		if (!(upwards > 0.0f) || !(length > Utils::MinWallNormalLength) || Utils::StandsOn(*character, body)
			|| !IsLighterThanCharacter(*m_Locks, body, character->GetMass()))
		{
			return;
		}
		const bool onLightGround = character->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround
			&& IsLighterThanCharacter(*m_Locks, character->GetGroundBodyID(), character->GetMass());
		if (onLightGround && !character->IsSlopeTooSteep(contactNormal))
			return;
		across /= length;
		const float into = characterVelocity.Dot(across);
		JPH::Vec3 velocity = characterVelocity - std::min(0.0f, into) * across;
		// What still moves into the contact's plane (a fall onto a steep face, with no motion into the body across) slides along it
		// instead: Jolt would meet the same contact again at once with nowhere to go, and the character would hang on the face.
		// The vertical part only moves towards zero (the normal faces upwards), so this never lifts the character.
		const float intoPlane = velocity.Dot(contactNormal);
		if (intoPlane < 0.0f)
			velocity -= intoPlane * contactNormal;
		newCharacterVelocity = velocity;
	}

	void CharacterContactRecorder::OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
		JPH::CharacterContactSettings& settings)
	{
		ApplyLightBodyRule(*character, contact, settings);

		// Jolt's contact normal points towards the character; the record's points from it to the body. A degenerate normal
		// (an exactly touching contact) falls back to straight down, where the character's support is.
		JPH::Vec3 normal = -contact.mContactNormal;
		if (!Utils::IsFinite(normal) || normal.LengthSq() < 0.25f)
			normal = -character->GetUp();
		else
			normal = normal.Normalized();
		const float speed = (character->GetLinearVelocity() - contact.mLinearVelocity).Dot(normal);

		m_Records.push_back(CharacterContactRecord{ .Kind = ContactEventKind::Added,
			.Body = contact.mBodyB,
			.SubShape = contact.mSubShapeIDB,
			.Point = Utils::IsFinite(contact.mPosition) ? contact.mPosition : character->GetPosition(),
			.Normal = normal,
			.RelativeNormalSpeed = std::isfinite(speed) ? speed : 0.0f });
	}

	void CharacterContactRecorder::OnContactRemoved(const JPH::CharacterVirtual* /*character*/, const JPH::BodyID& body, const JPH::SubShapeID& subShape)
	{
		m_Records.push_back(CharacterContactRecord{ .Kind = ContactEventKind::Removed, .Body = body, .SubShape = subShape });
	}

	std::vector<CharacterContactRecord> CharacterContactRecorder::Drain()
	{
		return std::exchange(m_Records, {});
	}

}
