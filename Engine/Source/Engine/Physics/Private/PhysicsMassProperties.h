#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/MassProperties.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

// The mass properties of Dynamic bodies (Architecture §9.1 "nothing invalid reaches Jolt"), shared by PhysicsWorld
// (CreateBody, SetShape) and PhysicsShape::CheckDynamicBody, so the edit-time validator and the world apply one rule.
// Private to the Physics module.

namespace Engine {

	namespace Detail {

		// The density, in kg/m^3, of every collider the Physics module builds (PhysicsShape::Create): Jolt's default of
		// 1000 kg/m^3 times 2^-64. No body's simulation uses a shape's own mass (a Dynamic body's mass properties are scaled
		// to its Mass, a Kinematic body gets fixed ones and a Static body none), but Jolt sums the colliders' masses and
		// moments when it builds a compound (its centre of mass) and when it computes a shape's inertia: each collider adds
		// its mass times its offset, and times the square of its offset, which at 1000 kg/m^3 reaches 1e36 kg m^2 for one
		// collider of PhysicsShape::MaxColliderSize at MaxPhysicsCoordinate, so a few hundred of them overflow a float. At
		// this density the sums stay finite for any compound of colliders PhysicsShape::Create accepts, and the smallest
		// collider's mass and inertia stay normal floats. Being a power of two, it scales each of Jolt's float products
		// exactly, so every mass ratio, centre of mass and inertia scaled to a body's Mass is the same, bit for bit, as at
		// Jolt's default density.
		inline constexpr float ColliderDensity = 1000.0f * 0x1p-64f;

		// The largest principal moment, in kg m^2, a Dynamic body may have: Jolt's check of its inertia decomposition
		// squares the inertia's columns, which must stay finite (FLT_MAX is about 3.4e38). A box of MaxPhysicsMass about 75 km
		// across reaches it, and so does a compound whose colliders lie far apart; anything larger is refused
		// (PhysicsShape::CheckDynamicBody).
		inline constexpr double MaxBodyInertia = 1.0e18;

		// The mass properties a Dynamic body of `mass` kg and `allowedDofs` gets from `shape`: the shape's, scaled to `mass`
		// as Jolt scales them (MassProperties::ScaleToMass, for EOverrideMassProperties::CalculateInertia), made safe for
		// Jolt's float eigen decomposition (MotionProperties::SetMassProperties), which asserts when its result is inaccurate:
		//   - exactly symmetric (a rotated collider's R * I * R^T computed in float is not);
		//   - well enough conditioned: a thin rod or needle (one principal moment far below the others) gets a little
		//     isotropic inertia, a thousandth of its trace, so its smallest moment is at least about a thousandth of its
		//     largest. Only such shapes change; their spin about the long axis becomes slightly slower to change.
		// Errors (PhysicsShape::CheckDynamicBody's): InvalidArgument for a mass that is not finite or outside
		// (0, MaxPhysicsMass]; Validation PHYSICS_NONCONVEX_DYNAMIC for a shape holding a triangle mesh (`hasMesh`);
		// Validation PHYSICS_ALL_DOFS_LOCKED for no degree of freedom, or for a body that cannot translate whose shape has no
		// rotational inertia; Validation PHYSICS_INVALID_SHAPE for a shape without a positive finite mass (no volume) or
		// whose inertia at `mass` is not finite or exceeds MaxBodyInertia.
		[[nodiscard]] Result<JPH::MassProperties> GetDynamicMassProperties(const JPH::Shape& shape, bool hasMesh, float mass, PhysicsDofs allowedDofs);

	}

}
