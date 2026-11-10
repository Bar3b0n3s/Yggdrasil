#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsMassProperties.h"

#include "Engine/Physics/PhysicsDiagnostics.h"

#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/StaticCompoundShape.h>

#include <array>
#include <cmath>
#include <utility>

namespace Engine {

	namespace {

		// The smallest ratio of an inertia tensor's determinant to its trace cubed that GetDynamicMassProperties leaves as it
		// is. The ratio bounds the principal moments: smallest / largest >= determinant / trace^3.
		constexpr double MinInertiaConditioning = 1.0e-3;

		// Whether Jolt can give a Dynamic body of `dofs` the mass properties `properties` without asserting
		// (MotionProperties::SetMassProperties): a body that cannot translate must have rotational inertia, or nothing could
		// move it.
		[[nodiscard]] bool CanTakeMassProperties(PhysicsDofs dofs, const JPH::MassProperties& properties)
		{
			constexpr PhysicsDofs Translations = PhysicsDofs::TranslationX | PhysicsDofs::TranslationY | PhysicsDofs::TranslationZ;
			if ((std::to_underlying(dofs) & std::to_underlying(Translations)) != 0)
				return true;
			JPH::Mat44 rotation;
			JPH::Vec3 diagonal;
			return properties.DecomposePrincipalMomentsOfInertia(rotation, diagonal) && !diagonal.IsNearZero();
		}

		// The sum over the spheres of `shape` (as AddDiscInertiaAboutZ finds them) of each one's mass at ColliderDensity times
		// its radius squared.
		[[nodiscard]] double SumSphereMassRadiusSquared(const JPH::Shape& shape)
		{
			switch (shape.GetSubType())
			{
				case JPH::EShapeSubType::Sphere:
				{
					const double radius = static_cast<const JPH::SphereShape&>(shape).GetRadius();
					return static_cast<double>(shape.GetMassProperties().mMass) * radius * radius;
				}
				case JPH::EShapeSubType::RotatedTranslated:
					return SumSphereMassRadiusSquared(*static_cast<const JPH::RotatedTranslatedShape&>(shape).GetInnerShape());
				case JPH::EShapeSubType::StaticCompound:
				{
					double sum = 0.0;
					for (const JPH::CompoundShape::SubShape& sub : static_cast<const JPH::StaticCompoundShape&>(shape).GetSubShapes())
						sum += SumSphereMassRadiusSquared(*sub.mShape);
					return sum;
				}
				default:
					return 0.0;
			}
		}

	}

	namespace Detail {

		Result<JPH::MassProperties> GetDynamicMassProperties(const JPH::Shape& shape, bool hasMesh, float mass, PhysicsDofs allowedDofs)
		{
			if (allowedDofs == PhysicsDofs::None)
				return MakeError(ErrorCode::Validation, "{}: a Dynamic body needs at least one degree of freedom", PhysicsAllDofsLockedCode);
			if (hasMesh)
			{
				return MakeError(ErrorCode::Validation, "{}: a Dynamic body cannot use a triangle mesh; use a convex hull or a Kinematic body",
					PhysicsNonconvexDynamicCode);
			}
			if (!std::isfinite(mass) || mass <= 0.0f || mass > MaxPhysicsMass)
				return MakeError(ErrorCode::InvalidArgument, "a Dynamic body's Mass must be above 0 and at most {} kg (got {})", MaxPhysicsMass, mass);

			JPH::MassProperties properties = shape.GetMassProperties();
			if (!std::isfinite(properties.mMass) || properties.mMass <= 0.0f)
				return MakeError(ErrorCode::Validation, "{}: the shape has no volume to give a Dynamic body its inertia", PhysicsInvalidShapeCode);
			properties.ScaleToMass(mass);

			std::array<std::array<double, 3>, 3> inertia{};
			for (uint32_t row = 0; row < 3; ++row)
			{
				for (uint32_t column = 0; column < 3; ++column)
					inertia[row][column] = 0.5 * (static_cast<double>(properties.mInertia(row, column)) + static_cast<double>(properties.mInertia(column, row)));
			}
			const double trace = inertia[0][0] + inertia[1][1] + inertia[2][2];
			const double determinant = inertia[0][0] * (inertia[1][1] * inertia[2][2] - inertia[1][2] * inertia[2][1])
				- inertia[0][1] * (inertia[1][0] * inertia[2][2] - inertia[1][2] * inertia[2][0])
				+ inertia[0][2] * (inertia[1][0] * inertia[2][1] - inertia[1][1] * inertia[2][0]);
			if (trace > 0.0 && determinant < MinInertiaConditioning * trace * trace * trace)
			{
				for (uint32_t axis = 0; axis < 3; ++axis)
					inertia[axis][axis] += MinInertiaConditioning * trace;
			}
			for (uint32_t row = 0; row < 3; ++row)
			{
				for (uint32_t column = 0; column < 3; ++column)
				{
					// A value that is not finite fails the comparison too.
					if (!(std::abs(inertia[row][column]) <= MaxBodyInertia))
					{
						return MakeError(ErrorCode::Validation, "{}: the shape is too large for a Dynamic body of {} kg: its inertia exceeds {} kg m^2",
							PhysicsInvalidShapeCode, mass, MaxBodyInertia);
					}
					properties.mInertia(row, column) = static_cast<float>(inertia[row][column]);
				}
			}
			properties.mInertia(3, 3) = 1.0f;
			if (!CanTakeMassProperties(allowedDofs, properties))
				return MakeError(ErrorCode::Validation, "{}: a Dynamic body that cannot translate needs a shape with rotational inertia", PhysicsAllDofsLockedCode);
			return properties;
		}

		void AddDiscInertiaAboutZ(const JPH::Shape& shape, JPH::MassProperties& properties)
		{
			const double spheres = SumSphereMassRadiusSquared(shape);
			const double shapeMass = shape.GetMassProperties().mMass;
			if (spheres <= 0.0 || !(shapeMass > 0.0))
				return;
			// The spheres' share of the body's mass, scaled as ScaleToMass scales the shape's moments.
			const double added = 0.1 * spheres * (static_cast<double>(properties.mMass) / shapeMass);
			properties.mInertia(2, 2) = static_cast<float>(static_cast<double>(properties.mInertia(2, 2)) + added);
		}

	}

}
