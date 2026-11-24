#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsContactListener.h"

#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsMassProperties.h"

#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/SubShapeIDPair.h>

#include <algorithm>
#include <cmath>

namespace Engine {

	namespace Detail {

		uint32_t ResolveColliderIndex(const JPH::Shape& shape, const JPH::SubShapeID& subShape, uint32_t singleCollider)
		{
			// M44: a mass override's wrapper consumes no sub-shape ID bits, so the ID names the wrapped shape's sub-shapes.
			const JPH::Shape& colliders = UnwrapBodyShape(shape);
			if (colliders.GetType() == JPH::EShapeType::Compound)
			{
				// The compound's own sub-shape user data, never GetSubShapeUserData: that is the leaf shape's, which colliders
				// sharing a cached shape have in common (PhysicsShape.h).
				const JPH::CompoundShape& compound = static_cast<const JPH::CompoundShape&>(colliders);
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
			const PhysicsBodyRecord* record = FindPhysicsBodyRecord(bodies, body.GetID());
			return ResolveColliderIndex(*body.GetShape(), subShape, record != nullptr ? record->Collider : 0);
		}

		float ResolveGroundFriction(const std::vector<PhysicsBodyRecord>& bodies, const JPH::Body& body, const JPH::SubShapeID& subShape)
		{
			const PhysicsBodyRecord* record = FindPhysicsBodyRecord(bodies, body.GetID());
			if (record != nullptr && !record->Surfaces.empty())
			{
				if (const PhysicsColliderSurface* surface = FindColliderSurface(*record, ResolvePhysicsCollider(bodies, body, subShape)))
					return surface->Friction;
			}
			return body.GetFriction();
		}

		const PhysicsBodyRecord* FindPhysicsBodyRecord(const std::vector<PhysicsBodyRecord>& bodies, const JPH::BodyID& body)
		{
			const uint32_t slot = body.GetIndex();
			return slot < bodies.size() && bodies[slot].ID == body ? &bodies[slot] : nullptr;
		}

		const PhysicsColliderSurface* FindColliderSurface(const PhysicsBodyRecord& record, uint32_t collider)
		{
			const auto found = std::lower_bound(record.Surfaces.begin(), record.Surfaces.end(), collider, [](const PhysicsColliderSurface& surface, uint32_t value)
			{
				return surface.Collider < value;
			});
			return found != record.Surfaces.end() && found->Collider == collider ? &*found : nullptr;
		}

		void PhysicsStepContacts::Reset()
		{
			const std::scoped_lock lock(m_Mutex);
			m_Contacts.clear();
		}

		std::pair<uint64_t, uint64_t> MakePhysicsContactKey(const JPH::BodyID& body1, const JPH::SubShapeID& subShape1, const JPH::BodyID& body2,
			const JPH::SubShapeID& subShape2)
		{
			const bool isOrdered = body1.GetIndexAndSequenceNumber() <= body2.GetIndexAndSequenceNumber();
			const uint32_t lowBody = isOrdered ? body1.GetIndexAndSequenceNumber() : body2.GetIndexAndSequenceNumber();
			const uint32_t highBody = isOrdered ? body2.GetIndexAndSequenceNumber() : body1.GetIndexAndSequenceNumber();
			const uint32_t lowSubShape = isOrdered ? subShape1.GetValue() : subShape2.GetValue();
			const uint32_t highSubShape = isOrdered ? subShape2.GetValue() : subShape1.GetValue();
			return { (static_cast<uint64_t>(lowBody) << 32) | highBody, (static_cast<uint64_t>(lowSubShape) << 32) | highSubShape };
		}

		void PhysicsStepContacts::Record(const JPH::BodyID& body1, const JPH::SubShapeID& subShape1, const JPH::BodyID& body2,
			const JPH::SubShapeID& subShape2)
		{
			const std::pair<uint64_t, uint64_t> key = MakePhysicsContactKey(body1, subShape1, body2, subShape2);
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

		bool PhysicsContactTouches(bool isSensor, float penetrationDepth, float relativeNormalSpeed, float collisionStepDelta)
		{
			if (isSensor || penetrationDepth >= 0.0f)
				return true;
			return relativeNormalSpeed * collisionStepDelta >= -penetrationDepth;
		}

		PhysicsContactListener::PhysicsContactListener(ContactBuffer& buffer, const std::vector<PhysicsBodyRecord>& bodies,
			PhysicsStepContacts& stepContacts)
			: m_Buffer(&buffer), m_Bodies(&bodies), m_StepContacts(&stepContacts)
		{
		}

		void PhysicsContactListener::SetCollisionStepDelta(float deltaTime)
		{
			m_CollisionStepDelta = deltaTime;
		}

		ContactEvent PhysicsContactListener::MakeRecord(ContactEventKind kind, const JPH::Body& body1, const JPH::Body& body2,
			const JPH::ContactManifold& manifold) const
		{
			// The first contact point (on body 1's surface), or the manifold's base when Jolt gave no point.
			const JPH::RVec3 point = manifold.mRelativeContactPointsOn1.empty() ? manifold.mBaseOffset : manifold.GetWorldSpaceContactPointOn1(0);
			// Jolt's normal points from body 1 towards body 2 (the direction that moves body 2 out of the collision), so body 1
			// approaches body 2 while their relative point velocity has a positive component along it.
			const JPH::Vec3 relativeVelocity = body1.GetPointVelocity(point) - body2.GetPointVelocity(point);

			ContactEvent event;
			event.Kind = kind;
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
			event.Touching = PhysicsContactTouches(event.IsSensor, manifold.mPenetrationDepth, event.RelativeNormalSpeed, m_CollisionStepDelta);
			return event;
		}

		void PhysicsContactListener::ApplySurfaces(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
			JPH::ContactSettings& settings) const
		{
			if (settings.mIsSensor)
				return;
			const PhysicsBodyRecord* record1 = FindPhysicsBodyRecord(*m_Bodies, body1.GetID());
			const PhysicsBodyRecord* record2 = FindPhysicsBodyRecord(*m_Bodies, body2.GetID());
			const bool listed1 = record1 != nullptr && !record1->Surfaces.empty();
			const bool listed2 = record2 != nullptr && !record2->Surfaces.empty();
			if (!listed1 && !listed2)
				return;
			const PhysicsColliderSurface* surface1 = listed1 ? FindColliderSurface(*record1, ResolvePhysicsCollider(*m_Bodies, body1, manifold.mSubShapeID1)) : nullptr;
			const PhysicsColliderSurface* surface2 = listed2 ? FindColliderSurface(*record2, ResolvePhysicsCollider(*m_Bodies, body2, manifold.mSubShapeID2)) : nullptr;
			if (surface1 == nullptr && surface2 == nullptr)
				return;
			// Jolt's own rules (ContactConstraintManager's default CombineFunctions) over each side's value, the geometric mean in
			// double precision: the product of two floats is exact in double, and the square root then rounded to float is the
			// correctly rounded one, so the result is a function of the two values alone (each at most MaxPhysicsFriction, so
			// no product overflows either way).
			const float friction1 = surface1 != nullptr ? surface1->Friction : body1.GetFriction();
			const float friction2 = surface2 != nullptr ? surface2->Friction : body2.GetFriction();
			const float restitution1 = surface1 != nullptr ? surface1->Restitution : body1.GetRestitution();
			const float restitution2 = surface2 != nullptr ? surface2->Restitution : body2.GetRestitution();
			settings.mCombinedFriction = static_cast<float>(std::sqrt(static_cast<double>(friction1) * static_cast<double>(friction2)));
			settings.mCombinedRestitution = std::max(restitution1, restitution2);
			// World space, body 2's surface velocity minus body 1's (ContactSettings::mRelativeLinearSurfaceVelocity).
			JPH::Vec3 velocity = JPH::Vec3::sZero();
			if (surface2 != nullptr)
				velocity += body2.GetRotation() * ToJolt(surface2->SurfaceVelocity);
			if (surface1 != nullptr)
				velocity -= body1.GetRotation() * ToJolt(surface1->SurfaceVelocity);
			settings.mRelativeLinearSurfaceVelocity = velocity;
		}

		void PhysicsContactListener::OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
			JPH::ContactSettings& settings)
		{
			ApplySurfaces(body1, body2, manifold, settings);
			m_StepContacts->Record(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
			const ContactEvent event = MakeRecord(ContactEventKind::Added, body1, body2, manifold);
			{
				// A contact Jolt adds again (after it removed it) starts over: remembered when it does not touch, forgotten
				// when it does.
				const ContactKey key = MakePhysicsContactKey(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
				const std::scoped_lock lock(m_SpeculativeMutex);
				if (event.Touching)
					m_Speculative.erase(key);
				else
					m_Speculative.insert(key);
				m_SpeculativeCount.store(m_Speculative.size(), std::memory_order_release);
			}
			m_Buffer->Append(event);
		}

		void PhysicsContactListener::OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold,
			JPH::ContactSettings& settings)
		{
			ApplySurfaces(body1, body2, manifold, settings);
			m_StepContacts->Record(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
			if (m_SpeculativeCount.load(std::memory_order_acquire) == 0)
				return;
			const ContactKey key = MakePhysicsContactKey(body1.GetID(), manifold.mSubShapeID1, body2.GetID(), manifold.mSubShapeID2);
			{
				const std::scoped_lock lock(m_SpeculativeMutex);
				if (!m_Speculative.contains(key))
					return;
			}
			const ContactEvent event = MakeRecord(ContactEventKind::Touched, body1, body2, manifold);
			if (!event.Touching)
				return;
			{
				// Jolt may report one contact persisted twice in a step (its CCD pass); only the call that forgets it records it.
				const std::scoped_lock lock(m_SpeculativeMutex);
				if (m_Speculative.erase(key) == 0)
					return;
				m_SpeculativeCount.store(m_Speculative.size(), std::memory_order_release);
			}
			m_Buffer->Append(event);
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
			if (m_SpeculativeCount.load(std::memory_order_acquire) != 0)
			{
				const ContactKey key = MakePhysicsContactKey(subShapePair.GetBody1ID(), subShapePair.GetSubShapeID1(), subShapePair.GetBody2ID(),
					subShapePair.GetSubShapeID2());
				const std::scoped_lock lock(m_SpeculativeMutex);
				m_Speculative.erase(key);
				m_SpeculativeCount.store(m_Speculative.size(), std::memory_order_release);
			}
			m_Buffer->Append(event);
		}

		bool PhysicsContactListener::IsRecordedSensor(const JPH::BodyID& body) const
		{
			const PhysicsBodyRecord* record = FindPhysicsBodyRecord(*m_Bodies, body);
			return record != nullptr && record->IsSensor;
		}

	}

}
