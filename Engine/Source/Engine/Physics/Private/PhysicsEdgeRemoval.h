#pragma once

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/Collision/ShapeFilter.h>

#include <numbers>

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

	}

}
