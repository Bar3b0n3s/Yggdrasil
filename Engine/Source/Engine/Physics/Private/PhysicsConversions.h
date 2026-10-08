#pragma once

#include "Engine/Core/Aabb.h"
#include "Engine/Core/Base.h"
#include "Engine/Physics/PhysicsTypes.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Jolt/Jolt.h must be included before any other Jolt header (Vendor/JoltPhysics/VENDOR.md).
#include <Jolt/Jolt.h>
#include <Jolt/Geometry/AABox.h>
#include <Jolt/Physics/Body/BodyID.h>

#include <cmath>
#include <optional>

// Conversions between the engine's types (glm, BodyHandle, Aabb) and Jolt's at the Physics module's boundary (§3 rule 3:
// Jolt types never reach a public header). Exact: every conversion copies floats and integers bit for bit, so poses written
// back to entities compare equal with what Jolt holds (Scene/PhysicsSystem's teleport detection relies on it). Private to the
// Physics module.

namespace Engine {

	namespace Detail {

		[[nodiscard]] inline JPH::Vec3 ToJolt(const glm::vec3& vector)
		{
			return JPH::Vec3(vector.x, vector.y, vector.z);
		}

		[[nodiscard]] inline glm::vec3 ToGlm(JPH::Vec3Arg vector)
		{
			return glm::vec3(vector.GetX(), vector.GetY(), vector.GetZ());
		}

		[[nodiscard]] inline glm::quat ToGlm(JPH::QuatArg rotation)
		{
			return glm::quat(rotation.GetW(), rotation.GetX(), rotation.GetY(), rotation.GetZ());
		}

		// A unit quaternion (IsPhysicsUnitRotation) as Jolt needs it: unchanged when Jolt counts it normalized, so a rotation
		// read back from Jolt goes in again bit for bit, else normalized.
		[[nodiscard]] inline JPH::Quat ToJoltRotation(const glm::quat& rotation)
		{
			const JPH::Quat converted(rotation.x, rotation.y, rotation.z, rotation.w);
			return converted.IsNormalized() ? converted : converted.Normalized();
		}

		[[nodiscard]] inline Aabb ToAabb(const JPH::AABox& box)
		{
			return Aabb{ .Min = ToGlm(box.mMin), .Max = ToGlm(box.mMax) };
		}

		[[nodiscard]] inline bool IsFinite(float value)
		{
			return std::isfinite(value);
		}

		[[nodiscard]] inline bool IsFinite(const glm::vec3& vector)
		{
			return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
		}

		[[nodiscard]] inline bool IsFinite(const glm::quat& rotation)
		{
			return std::isfinite(rotation.x) && std::isfinite(rotation.y) && std::isfinite(rotation.z) && std::isfinite(rotation.w);
		}

		// A finite vector whose squared length is within PhysicsUnitRotationTolerance of 1: the unit rule of rotations
		// (IsPhysicsUnitRotation, PhysicsTypes.h) for directions.
		[[nodiscard]] inline bool IsUnitVector(const glm::vec3& vector)
		{
			return IsFinite(vector) && std::abs(glm::dot(vector, vector) - 1.0f) <= PhysicsUnitRotationTolerance;
		}

		// The Jolt body a handle names; nullopt for the invalid handle and for values Jolt's BodyID cannot hold (its
		// broad-phase bit set), which no body of a world ever has.
		[[nodiscard]] inline std::optional<JPH::BodyID> ToJoltBodyID(BodyHandle body)
		{
			if (!body.IsValid() || (body.GetValue() & JPH::BodyID::cBroadPhaseBit) != 0)
				return std::nullopt;
			return JPH::BodyID(body.GetValue());
		}

		[[nodiscard]] inline BodyHandle ToBodyHandle(const JPH::BodyID& body)
		{
			return BodyHandle(body.GetIndexAndSequenceNumber());
		}

	}

}
