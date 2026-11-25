#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Physics/PhysicsTypes.h"

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Core/Reference.h>
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
		// (0, MaxPhysicsMass]; Validation PHYSICS_NONCONVEX_DYNAMIC for a shape holding a triangle mesh or a height field (`hasMesh`);
		// Validation PHYSICS_ALL_DOFS_LOCKED for no degree of freedom, or for a body that cannot translate whose shape has no
		// rotational inertia; Validation PHYSICS_INVALID_SHAPE for a shape without a positive finite mass (no volume) or
		// whose inertia at `mass` is not finite or exceeds MaxBodyInertia.
		[[nodiscard]] Result<JPH::MassProperties> GetDynamicMassProperties(const JPH::Shape& shape, bool hasMesh, float mass, PhysicsDofs allowedDofs);

		// M44 (Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; BodyDescription::MassOverride): the mass properties of
		// a Dynamic body whose shape is `shape` (the body's own, never the wrapper MakeDynamicBodyShape gives it) and whose
		// mass override is `massOverride`: the shape's, translated by CenterOfMassOffset when it is not zero (exactly the
		// OffsetCenterOfMassShape's GetMassProperties: MassProperties::Translate, the parallel axis theorem), scaled to `mass`,
		// then their inertia scaled by InertiaScale when it is not (1, 1, 1) (S^1/2 I S^1/2, in double), then made safe and
		// limited as above. The identity override takes exactly the four-argument function's path. Errors: the four-argument
		// function's, and InvalidArgument for an offset component that is not finite or above PhysicsShape::MaxColliderSize
		// in magnitude, or an InertiaScale component that is not finite or outside [MinPhysicsInertiaScale,
		// MaxPhysicsInertiaScale].
		[[nodiscard]] Result<JPH::MassProperties> GetDynamicMassProperties(const JPH::Shape& shape, bool hasMesh, float mass, PhysicsDofs allowedDofs,
			const PhysicsMassOverride& massOverride);

		// M44: the Jolt shape a Dynamic body of `shape` with `massOverride` is created with: `shape` itself for a zero
		// CenterOfMassOffset, else an OffsetCenterOfMassShape around it, which moves the centre of mass and passes every
		// sub-shape ID through unchanged (JPH::DecoratedShape). Errors: InvalidArgument for an offset that is not finite or out of
		// range; Validation PHYSICS_INVALID_SHAPE for a ShapeResult error (its text quoted).
		[[nodiscard]] Result<JPH::RefConst<JPH::Shape>> MakeDynamicBodyShape(const JPH::Shape& shape, const PhysicsMassOverride& massOverride);

		// M44: `shape` without the OffsetCenterOfMassShape a body's mass override wraps it in (MakeDynamicBodyShape): the
		// shape the body's colliders form, whose sub-shapes a compound's sub-shape IDs name. Any other shape is returned as it
		// is. Any thread: reads only the immutable shape.
		[[nodiscard]] const JPH::Shape& UnwrapBodyShape(const JPH::Shape& shape);

		// M21's milestone review (BodyDescription::SpheresAsDiscs): adds to `properties`, GetDynamicMassProperties' result for
		// `shape` (a Dynamic body's), the moment about Z that makes each sphere of `shape` turn about Z as a disc of its radius:
		// a tenth of its share of the body's mass times its radius squared, so its own moment is m r^2 / 2 instead of a
		// sphere's 2 m r^2 / 5. A sphere's share is its mass at ColliderDensity over the shape's, as Jolt shares a compound's
		// mass by volume. The spheres are found as PhysicsShape::Create builds them: the shape itself, the shape a
		// RotatedTranslatedShape places, or a sub-shape of a StaticCompoundShape; nothing else changes. Computed in double and
		// added to the inertia's Z diagonal (a sphere's own moment is the same about every axis, so neither its centre's place
		// nor the compound's rotations enter).
		void AddDiscInertiaAboutZ(const JPH::Shape& shape, JPH::MassProperties& properties);

	}

}
