#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsContactListener.h"

#include "Engine/Physics/Private/PhysicsConversions.h"

#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

namespace Engine {

	namespace Detail {

		uint32_t ResolveColliderIndex(const JPH::Shape& shape, const JPH::SubShapeID& subShape, uint32_t singleCollider)
		{
			if (shape.GetType() == JPH::EShapeType::Compound)
			{
				// The compound's own sub-shape user data, never GetSubShapeUserData: that is the leaf shape's, which colliders
				// sharing a cached shape have in common (PhysicsShape.h).
				const JPH::CompoundShape& compound = static_cast<const JPH::CompoundShape&>(shape);
				if (compound.IsSubShapeIDValid(subShape))
				{
					JPH::SubShapeID remainder;
					return compound.GetCompoundUserData(compound.GetSubShapeIndexFromID(subShape, remainder));
				}
			}
			return singleCollider;
		}

		uint32_t ResolvePhysicsCollider(const std::vector<PhysicsBodyRecord>& bodies, const JPH::Body& body, const JPH::SubShapeID& subShape)
		{
			const uint32_t slot = body.GetID().GetIndex();
			const bool hasRecord = slot < bodies.size() && bodies[slot].ID == body.GetID();
			return ResolveColliderIndex(*body.GetShape(), subShape, hasRecord ? bodies[slot].Collider : 0);
		}

		void PhysicsStepContacts::Reset()
		{
			const std::scoped_lock lock(m_Mutex);
			m_Contacts.clear();
		}

		void PhysicsStepContacts::Record(const JPH::BodyID& body1, const JPH::SubShapeID& subShape1, const JPH::BodyID& body2,
			const JPH::SubShapeID& subShape2)
		{
			const bool isOrdered = body1.GetIndexAndSequenceNumber() <= body2.GetIndexAndSequenceNumber();
			const uint32_t lowBody = isOrdered ? body1.GetIndexAndSequenceNumber() : body2.GetIndexAndSequenceNumber();
			const uint32_t highBody = isOrdered ? body2.GetIndexAndSequenceNumber() : body1.GetIndexAndSequenceNumber();
			const uint32_t lowSubShape = isOrdered ? subShape1.GetValue() : subShape2.GetValue();
			const uint32_t highSubShape = isOrdered ? subShape2.GetValue() : subShape1.GetValue();
			const std::pair<uint64_t, uint64_t> key((static_cast<uint64_t>(lowBody) << 32) | highBody, (static_cast<uint64_t>(lowSubShape) << 32) | highSubShape);

			const std::scoped_lock lock(m_Mutex);
			m_Contacts.push_back(key);
		}

		std::pair<uint32_t, uint32_t> PhysicsStepContacts::CountBodyPairsAndContacts()
		{
			const std::scoped_lock lock(m_Mutex);
			std::sort(m_Contacts.begin(), m_Contacts.end());
			m_Contacts.erase(std::unique(m_Contacts.begin(), m_Contacts.end()), m_Contacts.end());
			uint32_t bodyPairs = 0;
			for (size_t index = 0; index < m_Contacts.size(); ++index)
			{
				if (index == 0 || m_Contacts[index].first != m_Contacts[index - 1].first)
					++bodyPairs;
			}
			return { bodyPairs, static_cast<uint32_t>(m_Contacts.size()) };
		}

		PhysicsContactListener::PhysicsContactListener(ContactBuffer& buffer, const std::vector<PhysicsBodyRecord>& bodies,
			PhysicsStepContacts& stepContacts)
			: m_Buffer(&buffer), m_Bodies(&bodies), m_StepContacts(&stepContacts)
		{
		}

		void PhysicsContactListener::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
			JPH::ContactSettings& /*settings*/)
		{
			m_StepContacts->Record(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);

			// The first contact point (on body 1's surface), or the manifold's base when Jolt gave no point.
			const JPH::RVec3 point = manifold.mRelativeContactPointsOn1.empty() ? manifold.mBaseOffset : manifold.GetWorldSpaceContactPointOn1(0);
			// Jolt's normal points from body 1 towards body 2 (the direction that moves body 2 out of the collision), so body 1
			// approaches body 2 while their relative point velocity has a positive component along it.
			const JPH::Vec3 relativeVelocity = body1.GetPointVelocity(point) - body2.GetPointVelocity(point);

			ContactEvent event;
			event.Kind = ContactEventKind::Added;
			event.BodyA = ToBodyHandle(body1.GetID());
			event.BodyB = ToBodyHandle(body2.GetID());
			event.SubShapeA = manifold.mSubShapeID1.GetValue();
			event.SubShapeB = manifold.mSubShapeID2.GetValue();
			event.ColliderA = ResolvePhysicsCollider(*m_Bodies, body1, manifold.mSubShapeID1);
			event.ColliderB = ResolvePhysicsCollider(*m_Bodies, body2, manifold.mSubShapeID2);
			event.Point = ToGlm(point);
			event.Normal = ToGlm(manifold.mWorldSpaceNormal);
			event.RelativeNormalSpeed = relativeVelocity.Dot(manifold.mWorldSpaceNormal);
			event.IsSensor = body1.IsSensor() || body2.IsSensor();
			m_Buffer->Append(event);
		}

		void PhysicsContactListener::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
			JPH::ContactSettings& /*settings*/)
		{
			m_StepContacts->Record(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
		}

		void PhysicsContactListener::OnContactRemoved(const JPH::SubShapeIDPair& subShapePair)
		{
			ContactEvent event;
			event.Kind = ContactEventKind::Removed;
			event.BodyA = ToBodyHandle(subShapePair.GetBody1ID());
			event.BodyB = ToBodyHandle(subShapePair.GetBody2ID());
			event.SubShapeA = subShapePair.GetSubShapeID1().GetValue();
			event.SubShapeB = subShapePair.GetSubShapeID2().GetValue();
			event.IsSensor = IsRecordedSensor(subShapePair.GetBody1ID()) || IsRecordedSensor(subShapePair.GetBody2ID());
			m_Buffer->Append(event);
		}

		bool PhysicsContactListener::IsRecordedSensor(const JPH::BodyID& body) const
		{
			const uint32_t slot = body.GetIndex();
			return slot < m_Bodies->size() && (*m_Bodies)[slot].ID == body && (*m_Bodies)[slot].IsSensor;
		}

	}

}
