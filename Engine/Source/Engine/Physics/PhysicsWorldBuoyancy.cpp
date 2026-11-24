#include "EnginePCH.h"
#include "Engine/Physics/PhysicsWorld.h"

#include "Engine/Physics/Private/PhysicsConversions.h"
#include "Engine/Physics/Private/PhysicsWorldState.h"

#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/CompoundShape.h>
#include <Jolt/Physics/Collision/Shape/OffsetCenterOfMassShape.h>
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>

#include <cmath>
#include <optional>
#include <span>

// PhysicsWorld's buoyancy (M47, Docs/Decisions/0065-m47-water.md decision 3; Physics/PhysicsBuoyancy.h): each collider of a
// Dynamic body is submerged against its own fluid plane through Jolt's exact submerged volumes, the volumes and centres of
// buoyancy summed as CompoundShape::GetSubmergedVolume sums them, then one impulse through Jolt's precomputed-volume overload
// of Body::ApplyBuoyancyImpulse.

namespace Engine {

	namespace Utils {

		// A plane in water is finite, inside the world, and has a unit normal.
		static bool IsValidFluidPlane(const PhysicsFluidPlane& plane)
		{
			if (!plane.InWater)
				return true;
			if (!Detail::IsFinite(plane.Point) || !Detail::IsFinite(plane.Normal) || !Detail::IsFinite(plane.Velocity))
				return false;
			if (std::abs(plane.Point.x) > MaxPhysicsCoordinate || std::abs(plane.Point.y) > MaxPhysicsCoordinate || std::abs(plane.Point.z) > MaxPhysicsCoordinate)
				return false;
			const float lengthSquared = glm::dot(plane.Normal, plane.Normal);
			return std::abs(lengthSquared - 1.0f) <= PhysicsUnitRotationTolerance;
		}

		static bool IsWithin(float value, float low, float high)
		{
			return Detail::IsFinite(value) && value >= low && value <= high;
		}

		// The walk's inputs and sums: the planes; the body's centre of mass, the origin of the frame the planes are taken into
		// (world axes); the result, whose per-plane volumes it adds to; and the submerged-volume weighted sums of the centres of
		// buoyancy and the fluid velocities, before their division.
		struct BuoyancyWalk
		{
			std::span<const PhysicsFluidPlane> Planes{};
			JPH::Vec3 CenterOfMass = JPH::Vec3::sZero();
			PhysicsBuoyancyResult* Result = nullptr; // not owned
			float Total = 0.0f;
			float Submerged = 0.0f;
			JPH::Vec3 CenterOfBuoyancy = JPH::Vec3::sZero();
			JPH::Vec3 FluidVelocity = JPH::Vec3::sZero();
		};

		// How far below the centre of mass a collider out of water is submerged against: below any body the world holds, so it
		// gives its total volume and nothing submerged.
		constexpr float DryPlaneDepth = 1.0e6f;

		// Descends compound and decorating shapes (an offset centre of mass, a scale) to each collider's shape and submerges it
		// against its collider's plane; `userData` is the collider index of a shape that is not a compound's sub-shape.
		static void VisitBuoyancyShape(BuoyancyWalk& walk, const JPH::Shape& shape, JPH::Mat44Arg transform, JPH::Vec3Arg scale,
			uint32_t userData JPH_IF_DEBUG_RENDERER(, JPH::RVec3Arg base))
		{
			if (shape.GetType() == JPH::EShapeType::Compound)
			{
				for (const JPH::CompoundShape::SubShape& subShape : static_cast<const JPH::CompoundShape&>(shape).GetSubShapes())
				{
					VisitBuoyancyShape(walk, *subShape.mShape, transform * subShape.GetLocalTransformNoScale(scale), subShape.TransformScale(scale),
						subShape.mUserData JPH_IF_DEBUG_RENDERER(, base));
				}
				return;
			}
			if (shape.GetSubType() == JPH::EShapeSubType::OffsetCenterOfMass)
			{
				const auto& offset = static_cast<const JPH::OffsetCenterOfMassShape&>(shape);
				VisitBuoyancyShape(walk, *offset.GetInnerShape(), transform.PreTranslated(-scale * offset.GetOffset()), scale,
					userData JPH_IF_DEBUG_RENDERER(, base));
				return;
			}
			if (shape.GetSubType() == JPH::EShapeSubType::Scaled)
			{
				const auto& scaled = static_cast<const JPH::ScaledShape&>(shape);
				VisitBuoyancyShape(walk, *scaled.GetInnerShape(), transform, scale * scaled.GetScale(), userData JPH_IF_DEBUG_RENDERER(, base));
				return;
			}
			// One collider's shape (a rotated and translated shape places its own inner shape).
			const bool inWater = userData < walk.Planes.size() && walk.Planes[userData].InWater;
			JPH::Plane surface = JPH::Plane::sFromPointAndNormal(JPH::Vec3(0.0f, -DryPlaneDepth, 0.0f), JPH::Vec3::sAxisY());
			if (inWater)
			{
				const PhysicsFluidPlane& plane = walk.Planes[userData];
				surface = JPH::Plane::sFromPointAndNormal(Detail::ToJolt(plane.Point) - walk.CenterOfMass, Detail::ToJolt(plane.Normal).Normalized());
			}
			float total = 0.0f;
			float submerged = 0.0f;
			JPH::Vec3 center = JPH::Vec3::sZero();
			shape.GetSubmergedVolume(transform, scale, surface, total, submerged, center JPH_IF_DEBUG_RENDERER(, base));
			walk.Total += total;
			if (!inWater || !(submerged > 0.0f))
				return;
			walk.Submerged += submerged;
			walk.CenterOfBuoyancy += submerged * center;
			walk.FluidVelocity += submerged * Detail::ToJolt(walk.Planes[userData].Velocity);
			walk.Result->PlaneSubmergedVolumes[userData] += submerged;
		}

	}

	Result<PhysicsBuoyancyResult> PhysicsWorld::ApplyBuoyancy(BodyHandle body, std::span<const PhysicsFluidPlane> planes, const PhysicsBuoyancySettings& settings,
		float deltaTime)
	{
		if (!Detail::IsFinite(deltaTime) || !(deltaTime > 0.0f))
			return MakeError(ErrorCode::InvalidArgument, "buoyancy needs a finite, positive time step, got {}", deltaTime);
		if (!Utils::IsWithin(settings.Density, 1.0f, 20000.0f) || !Utils::IsWithin(settings.BuoyancyScale, 0.0f, 10.0f)
			|| !Utils::IsWithin(settings.LinearDrag, 0.0f, 100.0f) || !Utils::IsWithin(settings.AngularDrag, 0.0f, 100.0f))
		{
			return MakeError(ErrorCode::InvalidArgument, "buoyancy settings out of their bounds: density {}, scale {}, drags {} and {}", settings.Density,
				settings.BuoyancyScale, settings.LinearDrag, settings.AngularDrag);
		}
		for (size_t index = 0; index < planes.size(); ++index)
		{
			if (!Utils::IsValidFluidPlane(planes[index]))
				return MakeError(ErrorCode::InvalidArgument, "fluid plane {} is not finite, outside the world or has a normal that is not unit", index);
		}
		const Ref<const PhysicsShape> shape = IsBodyValid(body) ? GetShape(body) : nullptr;
		if (shape == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "body {} is not a live body of this world", body.GetValue());
		if (GetMotionType(body) != PhysicsMotionType::Dynamic)
			return MakeError(ErrorCode::InvalidArgument, "body {} is not Dynamic, so it cannot float", body.GetValue());
		const std::optional<JPH::BodyID> id = Detail::ToJoltBodyID(body);
		if (!id.has_value())
			return MakeError(ErrorCode::InvalidArgument, "body {} is not a live body of this world", body.GetValue());

		const JPH::BodyLockWrite lock(m_State->System->GetBodyLockInterface(), *id);
		if (!lock.Succeeded())
			return MakeError(ErrorCode::InvalidArgument, "body {} is not a live body of this world", body.GetValue());
		JPH::Body& joltBody = lock.GetBody();

		PhysicsBuoyancyResult result;
		result.PlaneSubmergedVolumes.assign(planes.size(), 0.0f);
		// The body's frame as Body::GetSubmergedVolume takes it: its rotation, its centre of mass at the origin.
		Utils::BuoyancyWalk walk{ .Planes = planes, .CenterOfMass = JPH::Vec3(joltBody.GetCenterOfMassPosition()), .Result = &result };
		const uint32_t firstCollider = shape->m_State->ColliderUserData.empty() ? 0 : shape->m_State->ColliderUserData.front();
		Utils::VisitBuoyancyShape(walk, *joltBody.GetShape(), JPH::Mat44::sRotation(joltBody.GetRotation()), JPH::Vec3::sOne(),
			firstCollider JPH_IF_DEBUG_RENDERER(, joltBody.GetCenterOfMassPosition()));
		result.TotalVolume = walk.Total;
		result.SubmergedVolume = walk.Submerged;
		if (!(walk.Submerged > 0.0f) || !(walk.Total > 0.0f))
			return result;

		// Jolt's fluid density is B / (V_total * invMass): B = scale * density * V_total * invMass makes it the water's, scaled.
		const float inverseMass = joltBody.GetMotionProperties()->GetInverseMass();
		const float buoyancy = settings.BuoyancyScale * settings.Density * walk.Total * inverseMass;
		const JPH::Vec3 centerOfBuoyancy = walk.CenterOfBuoyancy / walk.Submerged;
		const JPH::Vec3 fluidVelocity = walk.FluidVelocity / walk.Submerged;
		result.Submerged = joltBody.ApplyBuoyancyImpulse(walk.Total, walk.Submerged, centerOfBuoyancy, buoyancy, settings.LinearDrag, settings.AngularDrag,
			fluidVelocity, m_State->System->GetGravity(), deltaTime);
		return result;
	}

}
