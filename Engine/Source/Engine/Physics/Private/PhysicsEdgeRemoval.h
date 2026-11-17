#pragma once

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>
#include <Jolt/Physics/Collision/SimShapeFilter.h>

#include <cstdint>
#include <map>
#include <numbers>
#include <utility>

// The body-versus-body collision of every PhysicsWorld (JPH::PhysicsSystem::SetSimCollideBodyVsBody), for Architecture
// §9.2 "Seams": a rolling body with EnhancedInternalEdgeRemoval must cross the seams between the boxes of a track compound
// without a vertical bump (|v.y| < 0.05 m/s at 6 m/s).
//
// Jolt's own function collides such a body through its InternalEdgeRemovingCollector (Pierre Terdiman, "Contact generation
// for meshes"): a contact whose normal is within 1 degree of the touched face's normal counts as a face contact and is kept
// at once, voiding the face's vertices; any other is kept later only when the vertex or edge it touches was not voided by a
// deeper contact. 1 degree is too loose for the seam: a ball rolling at 6 m/s that touches the next box's leading edge at
// 0.5 degrees from vertical is kept as a face contact and bounces up at about 6 m/s * sin(0.5 degrees) = 0.05 m/s. This
// function runs the same algorithm with the face test at FaceContactAngle (0.1 degrees), where the bump stays below
// 0.011 m/s; pairs without enhanced internal edge removal collide exactly as Jolt's default does. The collector is adapted
// from Jolt's InternalEdgeRemovingCollector (Vendor/JoltPhysics/Jolt/Physics/Collision/InternalEdgeRemovingCollector.h,
// MIT licence, Vendor/JoltPhysics/LICENSE). Private to the Physics module.
//
// M37 (Docs/Decisions/0054-m37-joints.md decision 4): the hook is also where the world keeps a joint's two bodies apart
// while its CollideConnected is false. The collision groups' filter cannot (it sees groups, never bodies), so the world counts
// the joints between each body pair (PhysicsJointedPairs) and its body-pair collision drops such a pair before the narrow
// phase (CollideBodiesUnlessJointed). Jolt's continuous collision casts of LinearCast bodies do not go through the hook, so
// the world also installs PhysicsJointedPairFilter as its simulation shape filter while it holds such a pair; a world
// without one keeps Jolt's default filter, so its collisions are exactly what they were.

namespace Engine {

	namespace Detail {

		// The largest angle, in degrees, between a contact's normal and its face's normal for the contact to count as a face
		// contact.
		inline constexpr float FaceContactAngle = 0.1f;

		// cos(FaceContactAngle), the value the face test compares with: its Taylor series to the fourth power, computed at
		// compile time (no libm on the simulation path); the next term is below 1e-17 at this angle, so the float is exact.
		inline constexpr float FaceContactCosine = []
		{
			const double angle = static_cast<double>(FaceContactAngle) * std::numbers::pi / 180.0;
			const double squared = angle * angle;
			return static_cast<float>(1.0 - squared / 2.0 + squared * squared / 24.0);
		}();

		// A JPH::PhysicsSystem::SimCollideBodyVsBody: Jolt's default collision, with the internal edge removal described
		// above for pairs where either body sets EnhancedInternalEdgeRemoval. Called by Jolt's worker threads; it keeps no
		// state between calls.
		void CollideBodiesWithEdgeRemoval(const JPH::Body& body1, const JPH::Body& body2, JPH::Mat44Arg centerOfMassTransform1,
			JPH::Mat44Arg centerOfMassTransform2, JPH::CollideShapeSettings& settings, JPH::CollideShapeCollector& collector,
			const JPH::ShapeFilter& shapeFilter);

		// The body pairs the world's joints keep apart, each with the number of joints between them whose CollideConnected is
		// false (M37). Keyed by the two JPH::BodyID values (index and sequence number, so a destroyed body's pair never names
		// a body that reuses its index), the lower first. Changed on the main thread between steps; read by Jolt's worker
		// threads during a step, which only read it.
		class PhysicsJointedPairs
		{
		public:
			// Counts one more joint between `a` and `b`.
			void Add(const JPH::BodyID& a, const JPH::BodyID& b);
			// Counts one joint fewer between `a` and `b` (which Add counted), forgetting the pair at 0.
			void Remove(const JPH::BodyID& a, const JPH::BodyID& b);
			// Whether a joint keeps `a` and `b` apart.
			[[nodiscard]] bool Contains(const JPH::BodyID& a, const JPH::BodyID& b) const;
			[[nodiscard]] bool IsEmpty() const { return m_Counts.empty(); }
		private:
			[[nodiscard]] static std::pair<uint32_t, uint32_t> MakeKey(const JPH::BodyID& a, const JPH::BodyID& b);
		private:
			std::map<std::pair<uint32_t, uint32_t>, uint32_t> m_Counts{};
		};

		// The world's JPH::PhysicsSystem::SimCollideBodyVsBody (M37): nothing for a pair `pairs` holds, so no contact is made
		// between a joint's bodies; CollideBodiesWithEdgeRemoval otherwise. Called by Jolt's worker threads.
		void CollideBodiesUnlessJointed(const PhysicsJointedPairs& pairs, const JPH::Body& body1, const JPH::Body& body2,
			JPH::Mat44Arg centerOfMassTransform1, JPH::Mat44Arg centerOfMassTransform2, JPH::CollideShapeSettings& settings,
			JPH::CollideShapeCollector& collector, const JPH::ShapeFilter& shapeFilter);

		// The world's simulation shape filter while it holds a jointed pair (M37): refuses every shape of a pair `pairs` holds,
		// which keeps a LinearCast body's continuous collision cast from hitting the body it is joined to. Called by Jolt's
		// worker threads; reads `pairs` only. Not copyable (Jolt's filters are not).
		class PhysicsJointedPairFilter final : public JPH::SimShapeFilter
		{
		public:
			explicit PhysicsJointedPairFilter(const PhysicsJointedPairs& pairs)
				: m_Pairs(pairs)
			{
			}

			bool ShouldCollide(const JPH::Body& body1, const JPH::Shape* shape1, const JPH::SubShapeID& subShape1, const JPH::Body& body2,
				const JPH::Shape* shape2, const JPH::SubShapeID& subShape2) const override;
		private:
			// The world's pairs, a documented back-reference: the filter and the pairs are members of one PhysicsWorld state.
			const PhysicsJointedPairs& m_Pairs;
		};

	}

}
