#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/ContactBuffer.h"
#include "Engine/Physics/PhysicsShape.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/Shape/SubShapeID.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <utility>
#include <vector>

// The world's JPH::ContactListener (Architecture §9.4) and the per-body data it reads: Jolt calls it on its worker threads
// during PhysicsSystem::Update, and it only appends records to the world's ContactBuffer, counts the step's contacts for the
// limit diagnostics and remembers the speculative contacts that have not touched yet; nothing else happens on workers.
// Contact listener code runs while Jolt holds every body locked and, in MSVC Debug, with floating-point exceptions enabled,
// so it never locks a body and never produces NaN or Inf (§9.1). Private to the Physics module.
//
// Speculative contacts (Physics/ContactBuffer.h, Docs/Decisions/0031-m20-contract.md decision 3): an Added record carries
// whether the contact touches (PhysicsContactTouches); a contact added without touching is remembered until Jolt removes it
// or it touches, and at the first collision step in which OnContactPersisted finds it touching the listener appends one
// Touched record. Which contacts are remembered depends only on the contacts Jolt reports, never on the order its threads
// report them in, so the records stay deterministic once sorted.

namespace Engine {

	namespace Detail {

		// What the world keeps about a body it created (PhysicsWorld::CreateBody), indexed by the body's Jolt index. The main
		// thread writes it between steps; the contact listener reads it during steps. Every body of the world has one,
		// character inner bodies included; a slot whose ID is invalid or names another body is free.
		struct PhysicsBodyRecord
		{
			JPH::BodyID ID{};
			// The shape it was created with or last given (PhysicsWorld::GetShape).
			Ref<const PhysicsShape> Shape{};
			// BodyDescription::Mass of a Dynamic body, which PhysicsWorld::SetShape keeps.
			float Mass = 0.0f;
			// BodyDescription::CollisionGroup, which a new shape's CollisionGroup keeps.
			uint64_t CollisionGroup = 0;
			// The collider index every hit on a one-collider shape reports (ColliderShapeDescription::UserData); unused for a
			// compound, whose sub-shapes carry their own.
			uint32_t Collider = 0;
			bool IsSensor = false;
			// BodyDescription::MotionQuality was LinearCast: the body casts whenever its shape has an inner radius
			// (PhysicsWorld::SetShape decides again for a new shape).
			bool WantsLinearCast = false;
		};

		// The collider index of `subShape` on a body whose Jolt shape is `shape` (PhysicsShape.h "Collider identity"): for a
		// compound, the compound's sub-shape user data of the SubShapeID's first level (never the leaf's own user data, which
		// colliders sharing a cached shape have in common); for any other shape, or an ID that names no sub-shape of the
		// compound, `singleCollider` (the one collider's UserData). The one rule behind contacts, query hits and character
		// contacts. Any thread: reads only the immutable shape.
		[[nodiscard]] uint32_t ResolveColliderIndex(const JPH::Shape& shape, const JPH::SubShapeID& subShape, uint32_t singleCollider);

		// The collider index of `subShape` on `body` (ResolveColliderIndex with the one collider of the body's record; 0 for a
		// body without a record). Callable while Jolt holds the body locked (it only reads the body's shape and the records).
		[[nodiscard]] uint32_t ResolvePhysicsCollider(const std::vector<PhysicsBodyRecord>& bodies, const JPH::Body& body, const JPH::SubShapeID& subShape);

		// The key of a sub-shape contact: (body pair, sub-shape pair), each packed with the lower body ID's half first, so a
		// contact has one key whichever order Jolt reports its bodies in. Pure; any thread.
		[[nodiscard]] std::pair<uint64_t, uint64_t> MakePhysicsContactKey(const JPH::BodyID& body1, const JPH::SubShapeID& subShape1,
			const JPH::BodyID& body2, const JPH::SubShapeID& subShape2);

		// Whether a contact touches (Physics/ContactBuffer.h, Docs/Decisions/0031-m20-contract.md decision 3): a sensor contact
		// always (Jolt adds sensor contacts only for overlapping shapes); any other when its manifold's penetration depth is at
		// least 0, or when Jolt's solver acts on it within the collision step, its relative normal speed (positive when
		// approaching) times the collision step's delta time being at least its separation (the negated penetration depth).
		// Jolt applies a speculative contact's impulse and restitution by that test (ContactConstraintManager's
		// CalculateNonPenetrationConstraintProperties), so a body that bounces or stops on a speculative contact touches at
		// that step. Pure; any thread.
		[[nodiscard]] bool PhysicsContactTouches(bool isSensor, float penetrationDepth, float relativeNormalSpeed, float collisionStepDelta);

		// The contacts of the last step, counted by the listener for PhysicsWorldStats and the limit diagnostics: each
		// sub-shape contact (manifold) and each body pair that reported OnContactAdded or OnContactPersisted. Thread-safe.
		class PhysicsStepContacts
		{
		public:
			PhysicsStepContacts() = default;

			PhysicsStepContacts(const PhysicsStepContacts&) = delete;
			PhysicsStepContacts& operator=(const PhysicsStepContacts&) = delete;

			// Main thread, before a step.
			void Reset();
			// Any thread, during a step.
			void Record(const JPH::BodyID& body1, const JPH::SubShapeID& subShape1, const JPH::BodyID& body2, const JPH::SubShapeID& subShape2);
			// Main thread, after a step: the distinct body pairs and sub-shape contacts recorded since Reset.
			[[nodiscard]] std::pair<uint32_t, uint32_t> CountBodyPairsAndContacts();
		private:
			std::mutex m_Mutex; // guards m_Contacts
			// (body pair, sub-shape pair) of each record, the lower body ID first.
			std::vector<std::pair<uint64_t, uint64_t>> m_Contacts;
		};

		class PhysicsContactListener final : public JPH::ContactListener
		{
		public:
			// Documented back-references to members of the same PhysicsWorld, which outlive the listener's registration.
			PhysicsContactListener(ContactBuffer& buffer, const std::vector<PhysicsBodyRecord>& bodies, PhysicsStepContacts& stepContacts);

			PhysicsContactListener(const PhysicsContactListener&) = delete;
			PhysicsContactListener& operator=(const PhysicsContactListener&) = delete;

			// Main thread, before each world step: the delta time of one collision step (the step's delta over its collision
			// steps, as Jolt divides it), which the touching test uses.
			void SetCollisionStepDelta(float deltaTime);

			void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override;
			// Counts the contact; appends one Touched record when it is a speculative contact that touches now.
			void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold, JPH::ContactSettings& settings) override;
			void OnContactRemoved(const JPH::SubShapeIDPair& subShapePair) override;
		private:
			// A sub-shape contact (MakePhysicsContactKey).
			using ContactKey = std::pair<uint64_t, uint64_t>;

			// Whether the record of `body` says it is a sensor (Removed records, whose bodies Jolt does not let it touch).
			[[nodiscard]] bool IsRecordedSensor(const JPH::BodyID& body) const;
			// The Added or Touched record of a manifold, its Touching set by PhysicsContactTouches.
			[[nodiscard]] ContactEvent MakeRecord(ContactEventKind kind, const JPH::Body& body1, const JPH::Body& body2, const JPH::ContactManifold& manifold) const;
		private:
			ContactBuffer* m_Buffer = nullptr;
			const std::vector<PhysicsBodyRecord>* m_Bodies = nullptr;
			PhysicsStepContacts* m_StepContacts = nullptr;
			// Written on the main thread between steps, read by workers during them.
			float m_CollisionStepDelta = 1.0f / 60.0f;
			// The contacts added without touching that have neither touched nor been removed since.
			std::mutex m_SpeculativeMutex; // guards m_Speculative
			std::set<ContactKey> m_Speculative;
			// m_Speculative's size, so OnContactPersisted skips the lock while no contact waits (the common case). A contact's
			// own Added callback happens before its Persisted callbacks (Jolt's step phases are separated by job barriers), so
			// a stale count can only concern other contacts.
			std::atomic<size_t> m_SpeculativeCount = 0;
		};

	}

}
