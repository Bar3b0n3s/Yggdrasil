#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <mutex>
#include <vector>

// The contact buffer (Architecture §9.4): Jolt calls its ContactListener, and CharacterVirtual its
// CharacterContactListener (§9.6), on worker threads during PhysicsSystem::Update, in an order Jolt documents as
// non-deterministic. The listeners only append raw records here; nothing else happens on workers. After the step the main
// thread drains the buffer, and Scene/PhysicsSystem maps body handles to entities, collapses sub-shape contacts into body
// pairs with a reference count, sorts the events by the canonical order key (PhysicsEventOrderKey, Scene/PhysicsSystem.h)
// and dispatches them.
//
// Speculative contacts (M20, Docs/Decisions/0031-m20-contract.md decision 3). Jolt reports OnContactAdded for every
// contact manifold within its speculative contact distance, also while the two shapes are still apart. A sub-shape contact
// touches when its manifold's penetration depth is at least 0, or when Jolt's solver acts on it within the collision step:
// its RelativeNormalSpeed times the collision step's delta time (the world step's delta over its collision steps) is at
// least its separation (the negated penetration depth), Jolt's own test for applying a speculative contact's impulse and
// restitution. An Added record carries whether the contact touched when it was added (Touching). The world remembers each
// contact that was added without touching and appends one Touched record, from ContactListener::OnContactPersisted, at the
// first collision step in which it touches; a contact that is removed first never gets one. Character contacts
// (CharacterContactListener) are recorded touching.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 6); M20 owns the contact path in phase 1 and
// extends it (Touched, Touching; Docs/Decisions/0031-m20-contract.md decision 3).

namespace Engine {

	enum class ContactEventKind : uint8_t
	{
		Added,   // ContactListener::OnContactAdded, CharacterContactListener::OnContactAdded
		Removed, // ContactListener::OnContactRemoved, CharacterContactListener::OnContactRemoved
		Touched  // ContactListener::OnContactPersisted, once, for a contact added without touching that now touches (M20)
	};

	// One sub-shape contact that began or ended. BodyA and BodyB are as Jolt reported them (its own order, which is not
	// meaningful); the consumer orders the pair by entity UUID.
	struct ContactEvent
	{
		ContactEventKind Kind = ContactEventKind::Added;
		BodyHandle BodyA{};
		BodyHandle BodyB{};
		// Jolt's SubShapeID values of the pair: the key that the per-sub-shape reference count uses (§9.4 step 1).
		uint32_t SubShapeA = 0;
		uint32_t SubShapeB = 0;
		// Added and Touched only: the collider indices of the two sub-shapes (§9.2: the compound's sub-shape user data of the
		// SubShapeID's first level, or a one-collider body's only collider, PhysicsShape.h), read while Jolt holds the
		// bodies locked; 0 for Removed events, whose bodies may already be gone.
		uint32_t ColliderA = 0;
		uint32_t ColliderB = 0;
		// Added and Touched only: the first contact point in world space, the contact normal from A towards B (unit), and the
		// relative speed along it (positive when approaching), a Touched record's as Jolt reports the contact at the step it
		// first touches. Finite (contact listener code must not produce NaN/Inf, §9.1).
		glm::vec3 Point = glm::vec3(0.0f);
		glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		float RelativeNormalSpeed = 0.0f;
		// Whether either body is a sensor (a trigger pair, §9.4: OnTriggerEnter/Exit instead of OnCollision*).
		bool IsSensor = false;
		// Whether the record came from a character's CharacterContactListener (BodyA is then the character's inner body,
		// §9.6).
		bool FromCharacter = false;
		// Added: whether the contact touched when Jolt added it (see the file comment); false for a speculative contact,
		// which then gets a Touched record when it first touches. Touched records are always true; Removed records leave it
		// at its default. M20 (W-P03).
		bool Touching = true;

		bool operator==(const ContactEvent&) const = default;
	};

	// A mutex-guarded append-only buffer. Append is thread-safe; Drain and Clear run on the main thread between steps.
	class ContactBuffer
	{
	public:
		ContactBuffer() = default;

		ContactBuffer(const ContactBuffer&) = delete;
		ContactBuffer& operator=(const ContactBuffer&) = delete;

		// Appends one record (any thread).
		void Append(const ContactEvent& event);
		// The records appended since the last Drain, in append order (which differs between runs and thread counts; the
		// consumer sorts), and empties the buffer.
		[[nodiscard]] std::vector<ContactEvent> Drain();
		// Discards every record.
		void Clear();
		// The number of records held.
		[[nodiscard]] size_t GetSize() const;
	private:
		mutable std::mutex m_Mutex; // guards m_Events
		std::vector<ContactEvent> m_Events;
	};

}
