#pragma once

#include "Engine/Core/Base.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/CollisionGroup.h>
#include <Jolt/Physics/Collision/GroupFilter.h>

#include <cstdint>

// The collision group rule of one PhysicsWorld (PhysicsWorld.h): bodies of the same non-zero BodyDescription::CollisionGroup
// never collide or report each other, and two bodies whose shapes both hold a triangle mesh never meet (Jolt has no
// mesh-versus-mesh collision and asserts on such a pair, which a mesh sensor and a Kinematic mesh body could form). Jolt asks
// a body pair's GroupFilter when the broad phase finds the pair, before the narrow phase (Body::sFindCollidingPairsCanCollide
// and the CCD cast), so a refused pair never reaches a collision function. Queries do not consult it.
//
// A body's JPH::CollisionGroup names one of the world's two filter instances, the mesh one for bodies whose shape holds a
// mesh, and carries the 64-bit group in GroupID (low half) and SubGroupID (high half). Every body of the world is created
// with one (PhysicsWorld::CreateBody, character inner bodies included); a body without one of these filters has no group
// and holds no mesh. Private to the Physics module.

namespace Engine {

	namespace Detail {

		class PhysicsCollisionGroups;

		// One of the two JPH::GroupFilter instances of a PhysicsCollisionGroups: embedded in it (never deleted through a
		// reference count), with a documented back-reference to its owner, which answers the question.
		class PhysicsGroupFilter final : public JPH::GroupFilter
		{
		public:
			explicit PhysicsGroupFilter(const PhysicsCollisionGroups& groups);

			PhysicsGroupFilter(const PhysicsGroupFilter&) = delete;
			PhysicsGroupFilter& operator=(const PhysicsGroupFilter&) = delete;

			[[nodiscard]] bool CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const override;
		private:
			const PhysicsCollisionGroups* m_Groups = nullptr;
		};

		// The rule and the two filters. Thread-safe to read (Jolt's worker threads call CanCollide during a step); it never
		// changes after construction. Every body holding one of its groups must be destroyed before it is (the embedded
		// filters assert that no reference is left).
		class PhysicsCollisionGroups
		{
		public:
			PhysicsCollisionGroups();

			PhysicsCollisionGroups(const PhysicsCollisionGroups&) = delete;
			PhysicsCollisionGroups& operator=(const PhysicsCollisionGroups&) = delete;

			// The JPH::CollisionGroup of a body of `group` (0: none) whose shape does or does not hold a triangle mesh.
			[[nodiscard]] JPH::CollisionGroup MakeCollisionGroup(uint64_t group, bool holdsMesh) const;

			// False for two bodies of the same non-zero group and for two bodies that both hold a mesh; true otherwise,
			// and for every pair with a body whose group is not one of this world's.
			[[nodiscard]] bool CanCollide(const JPH::CollisionGroup& group1, const JPH::CollisionGroup& group2) const;
		private:
			[[nodiscard]] bool IsOwnGroup(const JPH::CollisionGroup& group) const;
		private:
			PhysicsGroupFilter m_SolidFilter;
			PhysicsGroupFilter m_MeshFilter;
		};

	}

}
