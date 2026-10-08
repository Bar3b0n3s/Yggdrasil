#include "EnginePCH.h"
#include "Engine/Physics/Private/PhysicsMassProperties.h"

#include "Engine/Physics/PhysicsDiagnostics.h"

#include <Jolt/Math/Mat44.h>
#include <Jolt/Math/Vec3.h>

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

	}

}
