#include "EnginePCH.h"
#include "Engine/Scene/TransformSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Scene/Components/RelationshipComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

// Simulation path (§4.12): only + - * / and sqrt (glm's matrix products, inverses and quat_cast are built from them) and
// Core/DetMath; no CRT transcendental function, so every result is bit-identical across platforms and configurations.

namespace Engine {

	namespace Utils {

		constexpr double DegreesToRadians = 0.017453292519943295769236907684886; // pi / 180
		constexpr double RadiansToDegrees = 57.295779513082320876798154814105;   // 180 / pi
		constexpr std::array<char, 3> AxisNames = { 'X', 'Y', 'Z' };

		// cos(theta) above which slerp falls back to a normalized lerp: sin(theta) is too small to divide by.
		constexpr double SlerpLinearThreshold = 0.9995;
		// |cos X| below which the Euler extraction is in gimbal lock (X within about 6e-5 degrees of +-90, the rounding of a
		// float quaternion) and puts the whole rotation about the vertical axis into Y.
		constexpr double GimbalLockCosine = 1e-6;
		// The smallest |det| of the column-normalized 3x3 that DecomposeMatrix accepts: below it the columns are (almost)
		// linearly dependent and no rotation is meaningful.
		constexpr double MinNormalizedDeterminant = 1e-6;
		// Polar decomposition (closest rotation): iteration limit and the element change at which it has converged.
		constexpr int MaxPolarIterations = 16;
		constexpr double PolarConvergence = 1e-12;

		// The entity and its ancestors, root first.
		static std::vector<ConstEntity> CollectChain(ConstEntity entity)
		{
			std::vector<ConstEntity> chain;
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
				chain.push_back(current);
			std::ranges::reverse(chain);
			return chain;
		}

		static bool IsFinite(const glm::mat4& matrix)
		{
			for (glm::length_t column = 0; column < 4; ++column)
			{
				for (glm::length_t row = 0; row < 4; ++row)
				{
					if (!std::isfinite(matrix[column][row]))
						return false;
				}
			}
			return true;
		}

		static glm::quat ToFloat(const glm::dquat& rotation)
		{
			return glm::quat(static_cast<float>(rotation.w), static_cast<float>(rotation.x), static_cast<float>(rotation.y),
				static_cast<float>(rotation.z));
		}

		static glm::dquat ToDouble(const glm::quat& rotation)
		{
			return glm::dquat(rotation.w, rotation.x, rotation.y, rotation.z);
		}

		// The length of `rotation` in double precision, as the Quat field rule measures it (UnitQuaternionTolerance). Only
		// asserts call it, so it is unused where they compile out (Dist).
		[[maybe_unused]] static double GetQuaternionLength(const glm::quat& rotation)
		{
			const glm::dquat value = ToDouble(rotation);
			return std::sqrt(glm::dot(value, value));
		}

		// The rotation closest to `matrix` (Frobenius norm): Newton's polar iteration R <- (R + R^-T) / 2, which converges
		// quadratically for a non-singular matrix with a positive determinant and leaves a rotation unchanged up to rounding.
		static glm::dmat3 ClosestRotation(const glm::dmat3& matrix)
		{
			glm::dmat3 rotation = matrix;
			for (int iteration = 0; iteration < MaxPolarIterations; ++iteration)
			{
				const glm::dmat3 next = (rotation + glm::transpose(glm::inverse(rotation))) * 0.5;
				double change = 0.0;
				for (glm::length_t column = 0; column < 3; ++column)
				{
					for (glm::length_t row = 0; row < 3; ++row)
						change = std::max(change, std::abs(next[column][row] - rotation[column][row]));
				}
				rotation = next;
				if (change <= PolarConvergence)
					break;
			}
			return rotation;
		}

		// Spherical interpolation through DetMath, along the shorter arc; `alpha` in [0, 1].
		static glm::quat Slerp(const glm::quat& from, const glm::quat& to, float alpha)
		{
			if (alpha <= 0.0f)
				return from;
			if (alpha >= 1.0f)
				return to;

			const glm::dquat start = ToDouble(from);
			glm::dquat end = ToDouble(to);
			double cosTheta = glm::dot(start, end);
			if (cosTheta < 0.0)
			{
				end = -end;
				cosTheta = -cosTheta;
			}

			const double t = alpha;
			double startWeight = 1.0 - t;
			double endWeight = t;
			if (cosTheta < SlerpLinearThreshold)
			{
				const double theta = DetMath::ACos(cosTheta);
				const double sinTheta = DetMath::Sin(theta);
				startWeight = DetMath::Sin((1.0 - t) * theta) / sinTheta;
				endWeight = DetMath::Sin(t * theta) / sinTheta;
			}
			return ToFloat(glm::normalize(start * startWeight + end * endWeight));
		}

		// The previous world matrix to interpolate from, or nullptr when the entity renders at its current pose (§5.2):
		// outside play, when it or an ancestor has InterpolationResetTag, or before its first snapshot.
		static const PreviousWorldTransformComponent* FindInterpolationStart(ConstEntity entity)
		{
			if (!entity.GetScene()->IsRuntime())
				return nullptr;
			for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
			{
				if (current.HasComponent<InterpolationResetTag>())
					return nullptr;
			}
			return entity.TryGetComponent<PreviousWorldTransformComponent>();
		}

		// An angle in degrees in (-180, 180] from atan2's [-pi, pi].
		static float ToHalfOpenDegrees(double radians)
		{
			double degrees = radians * RadiansToDegrees;
			if (degrees <= -180.0)
				degrees += 360.0;
			return static_cast<float>(degrees);
		}

	}

	void TransformSystem::Update(Scene& scene)
	{
		entt::registry& registry = scene.GetRegistry();
		// Parents precede their children in canonical order, so each parent's world matrix is already current.
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const entt::entity handle = scene.FindEntityByID(id).GetHandle();
			glm::mat4 world = ComputeLocalMatrix(registry.get<TransformComponent>(handle));
			const UUID parent = registry.get<RelationshipComponent>(handle).Parent;
			if (parent.IsValid())
				world = registry.get<WorldTransformComponent>(scene.FindEntityByID(parent).GetHandle()).Matrix * world;
			registry.emplace_or_replace<WorldTransformComponent>(handle, world);
		}
	}

	glm::mat4 TransformSystem::ComputeLocalMatrix(const TransformComponent& transform)
	{
		const glm::mat3 rotation = glm::mat3_cast(transform.Rotation);
		glm::mat4 matrix(1.0f);
		matrix[0] = glm::vec4(rotation[0] * transform.Scale.x, 0.0f);
		matrix[1] = glm::vec4(rotation[1] * transform.Scale.y, 0.0f);
		matrix[2] = glm::vec4(rotation[2] * transform.Scale.z, 0.0f);
		matrix[3] = glm::vec4(transform.Translation, 1.0f);
		return matrix;
	}

	glm::mat4 TransformSystem::ComputeWorldMatrix(ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "ComputeWorldMatrix needs a valid entity");
		// The same products in the same order as Update (root local first, then ParentWorld * Local), so both give
		// bit-identical matrices.
		const std::vector<ConstEntity> chain = Utils::CollectChain(entity);
		glm::mat4 world = ComputeLocalMatrix(chain.front().GetComponent<TransformComponent>());
		for (size_t index = 1; index < chain.size(); ++index)
			world = world * ComputeLocalMatrix(chain[index].GetComponent<TransformComponent>());
		return world;
	}

	Result<TransformDecomposition> TransformSystem::DecomposeMatrix(const glm::mat4& matrix)
	{
		if (!Utils::IsFinite(matrix))
			return MakeError(ErrorCode::InvalidArgument, "the matrix has a non-finite element and cannot be decomposed");

		const std::array<glm::dvec3, 3> columns = {
			glm::dvec3(glm::vec3(matrix[0])),
			glm::dvec3(glm::vec3(matrix[1])),
			glm::dvec3(glm::vec3(matrix[2])),
		};
		glm::dvec3 scale(glm::length(columns[0]), glm::length(columns[1]), glm::length(columns[2]));
		for (glm::length_t axis = 0; axis < 3; ++axis)
		{
			if (scale[axis] < static_cast<double>(MinTransformScaleMagnitude))
			{
				return MakeError(ErrorCode::InvalidArgument, "scale component {} of the matrix has magnitude {}, below the minimum {}",
					Utils::AxisNames[static_cast<size_t>(axis)], scale[axis], MinTransformScaleMagnitude);
			}
			if (scale[axis] > static_cast<double>(std::numeric_limits<float>::max()))
			{
				return MakeError(ErrorCode::InvalidArgument, "scale component {} of the matrix has magnitude {}, beyond the float range",
					Utils::AxisNames[static_cast<size_t>(axis)], scale[axis]);
			}
		}

		glm::dmat3 normalized(columns[0] / scale.x, columns[1] / scale.y, columns[2] / scale.z);
		const double determinant = glm::determinant(normalized);
		if (std::abs(determinant) < Utils::MinNormalizedDeterminant)
			return MakeError(ErrorCode::InvalidArgument, "the matrix is degenerate: its axes are linearly dependent");
		if (determinant < 0.0)
		{
			// A mirroring matrix: the reflection is expressed as a negative X scale, the rest is a rotation.
			scale.x = -scale.x;
			normalized[0] = -normalized[0];
		}

		glm::dquat rotation = glm::normalize(glm::quat_cast(Utils::ClosestRotation(normalized)));
		if (rotation.w < 0.0)
			rotation = -rotation;

		// Rounding to float keeps every magnitude within [MinTransformScaleMagnitude, float max]: both bounds are floats and
		// rounding is monotonic.
		TransformDecomposition decomposition;
		decomposition.Translation = glm::vec3(matrix[3]);
		decomposition.Rotation = Utils::ToFloat(rotation);
		decomposition.Scale = glm::vec3(scale);
		return decomposition;
	}

	glm::vec3 TransformSystem::GetWorldPosition(ConstEntity entity)
	{
		return glm::vec3(ComputeWorldMatrix(entity)[3]);
	}

	void TransformSystem::SetWorldPosition(Entity entity, const glm::vec3& position)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "SetWorldPosition needs a valid entity");
		ENGINE_CORE_ASSERT(!glm::any(glm::isnan(position)) && !glm::any(glm::isinf(position)), "SetWorldPosition needs a finite position");

		glm::vec3 local = position;
		if (const Entity parent = entity.GetParent(); parent.IsValid())
			local = glm::vec3(glm::affineInverse(ComputeWorldMatrix(parent)) * glm::vec4(position, 1.0f));
		ENGINE_CORE_ASSERT(!glm::any(glm::isnan(local)) && !glm::any(glm::isinf(local)),
			"The parent of entity {} has a world transform that cannot be inverted", entity.GetUUID());

		entity.Patch<TransformComponent>([&local](TransformComponent& transform)
		{
			transform.Translation = local;
		});
	}

	glm::quat TransformSystem::GetWorldRotation(ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "GetWorldRotation needs a valid entity");
		const std::vector<ConstEntity> chain = Utils::CollectChain(entity);
		glm::quat rotation = chain.front().GetComponent<TransformComponent>().Rotation;
		for (size_t index = 1; index < chain.size(); ++index)
			rotation = rotation * chain[index].GetComponent<TransformComponent>().Rotation;
		return glm::normalize(rotation);
	}

	void TransformSystem::SetWorldRotation(Entity entity, const glm::quat& rotation)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "SetWorldRotation needs a valid entity");
		// The comparison is false for NaN and infinite components too.
		ENGINE_CORE_ASSERT(std::abs(Utils::GetQuaternionLength(rotation) - 1.0) <= UnitQuaternionTolerance,
			"SetWorldRotation needs a finite unit quaternion");

		glm::quat local = rotation;
		if (const Entity parent = entity.GetParent(); parent.IsValid())
			local = glm::normalize(glm::conjugate(GetWorldRotation(parent)) * rotation);

		entity.Patch<TransformComponent>([&local](TransformComponent& transform)
		{
			transform.Rotation = local;
		});
	}

	glm::vec3 TransformSystem::GetWorldScale(ConstEntity entity)
	{
		ENGINE_CORE_ASSERT(entity.IsValid(), "GetWorldScale needs a valid entity");
		const glm::mat4 world = ComputeWorldMatrix(entity);
		glm::vec3 scale(glm::length(glm::vec3(world[0])), glm::length(glm::vec3(world[1])), glm::length(glm::vec3(world[2])));
		for (ConstEntity current = entity; current.IsValid(); current = current.GetParent())
		{
			const glm::vec3& local = current.GetComponent<TransformComponent>().Scale;
			for (glm::length_t axis = 0; axis < 3; ++axis)
			{
				if (local[axis] < 0.0f)
					scale[axis] = -scale[axis];
			}
		}
		return scale;
	}

	glm::vec3 TransformSystem::GetRenderPosition(ConstEntity entity)
	{
		const glm::vec3 current = GetWorldPosition(entity);
		const PreviousWorldTransformComponent* previous = Utils::FindInterpolationStart(entity);
		const float alpha = entity.GetScene()->GetInterpolationAlpha();
		if (previous == nullptr || alpha >= 1.0f)
			return current;

		const glm::vec3 start(previous->Matrix[3]);
		return start + (current - start) * alpha;
	}

	glm::quat TransformSystem::GetRenderRotation(ConstEntity entity)
	{
		const glm::quat current = GetWorldRotation(entity);
		const PreviousWorldTransformComponent* previous = Utils::FindInterpolationStart(entity);
		const float alpha = entity.GetScene()->GetInterpolationAlpha();
		if (previous == nullptr || alpha >= 1.0f)
			return current;

		const Result<TransformDecomposition> start = DecomposeMatrix(previous->Matrix);
		if (!start.has_value())
			return current; // a degenerate previous pose cannot be interpolated from
		return Utils::Slerp(start->Rotation, current, alpha);
	}

	glm::quat TransformSystem::QuaternionFromEulerDegrees(const glm::vec3& degrees)
	{
		const SinCosResult<double> x = DetMath::SinCos(static_cast<double>(degrees.x) * Utils::DegreesToRadians * 0.5);
		const SinCosResult<double> y = DetMath::SinCos(static_cast<double>(degrees.y) * Utils::DegreesToRadians * 0.5);
		const SinCosResult<double> z = DetMath::SinCos(static_cast<double>(degrees.z) * Utils::DegreesToRadians * 0.5);
		const glm::dquat aboutX(x.Cos, x.Sin, 0.0, 0.0);
		const glm::dquat aboutY(y.Cos, 0.0, y.Sin, 0.0);
		const glm::dquat aboutZ(z.Cos, 0.0, 0.0, z.Sin);
		// Z first, then X, then Y applied to a vector.
		return Utils::ToFloat(glm::normalize(aboutY * aboutX * aboutZ));
	}

	glm::vec3 TransformSystem::EulerDegreesFromQuaternion(const glm::quat& rotation)
	{
		// The rotation matrix of q = qY * qX * qZ is R = Ry * Rx * Rz (Rij: row i, column j), whose elements give
		//   R12 = -sin X, (R02, R22) = cos X * (sin Y, cos Y), (R10, R11) = cos X * (sin Z, cos Z).
		const glm::dquat q = glm::normalize(Utils::ToDouble(rotation));
		const double r00 = 1.0 - 2.0 * (q.y * q.y + q.z * q.z);
		const double r02 = 2.0 * (q.x * q.z + q.w * q.y);
		const double r10 = 2.0 * (q.x * q.y + q.w * q.z);
		const double r12 = 2.0 * (q.y * q.z - q.w * q.x);
		const double r20 = 2.0 * (q.x * q.z - q.w * q.y);
		const double r22 = 1.0 - 2.0 * (q.x * q.x + q.y * q.y);

		// cos X from the column that keeps full precision near +-90 degrees, where asin(-R12) would not.
		const double sinX = -r12;
		const double cosX = std::sqrt(r02 * r02 + r22 * r22);
		const double angleX = DetMath::ATan2(sinX, cosX);
		if (cosX < Utils::GimbalLockCosine)
		{
			// Gimbal lock: Y and Z rotate about the same axis. Z is 0 and Y takes the whole rotation, from
			// (R00, R20) = (cos Y, -sin Y).
			const double angleY = DetMath::ATan2(-r20, r00);
			return glm::vec3(static_cast<float>(angleX * Utils::RadiansToDegrees), Utils::ToHalfOpenDegrees(angleY), 0.0f);
		}

		// Z is extracted from Rz = Rx^T * Ry^T * R with the X and Y just found, so any rounding in Y is compensated in Z and
		// the angles reproduce the rotation even close to the lock.
		const double angleY = DetMath::ATan2(r02, r22);
		const double sinY = r02 / cosX;
		const double cosY = r22 / cosX;
		const double norm = std::sqrt(sinX * sinX + cosX * cosX);
		const double sinXUnit = sinX / norm;
		const double cosXUnit = cosX / norm;
		const double cosZ = cosY * r00 - sinY * r20;
		const double sinZ = cosXUnit * r10 + sinXUnit * (sinY * r00 + cosY * r20);
		const double angleZ = DetMath::ATan2(sinZ, cosZ);
		return glm::vec3(static_cast<float>(angleX * Utils::RadiansToDegrees), Utils::ToHalfOpenDegrees(angleY),
			Utils::ToHalfOpenDegrees(angleZ));
	}

}
