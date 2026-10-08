#include "EnginePCH.h"
#include "Engine/Physics/Private/CharacterContactRecorder.h"

#include <cmath>
#include <utility>

namespace Engine {

	namespace Utils {

		[[nodiscard]] static bool IsFinite(JPH::Vec3Arg value)
		{
			return std::isfinite(value.GetX()) && std::isfinite(value.GetY()) && std::isfinite(value.GetZ());
		}

	}

	bool CharacterContactRecorder::OnContactValidate(const JPH::CharacterVirtual* /*character*/, const JPH::CharacterContact& contact)
	{
		return !contact.mIsSensorB;
	}

	void CharacterContactRecorder::OnContactAdded(const JPH::CharacterVirtual* character, const JPH::CharacterContact& contact,
		JPH::CharacterContactSettings& /*settings*/)
	{
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
