#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cmath>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <string_view>

// The vocabulary of the physics module (Architecture §9.1): body handles, motion types, degrees of freedom, layer masks
// and the world's limits, in engine types. Jolt's own types never appear in a public header (§3 rule 3); the Physics module
// converts at its boundary (Physics/Private/). Plain value types; thread-compatible.
//
// Frozen by the M11 contract (Docs/Decisions/0014-m11-decisions.md decision 2).

namespace Engine {

	// One body of a PhysicsWorld: Jolt's BodyID value (index and sequence number), so a handle of a destroyed body never
	// names the body that later reuses its index. The default handle is invalid. Handles are compared and ordered by value,
	// which is the order the world created them in only while no index was reused; deterministic ordering uses the owning
	// entity's UUID (§9.4), never handles.
	class BodyHandle
	{
	public:
		// Jolt's BodyID::cInvalidBodyID.
		static constexpr uint32_t InvalidValue = 0xffffffffu;

		constexpr BodyHandle() = default;
		explicit constexpr BodyHandle(uint32_t value)
			: m_Value(value)
		{
		}

		[[nodiscard]] constexpr uint32_t GetValue() const { return m_Value; }
		[[nodiscard]] constexpr bool IsValid() const { return m_Value != InvalidValue; }

		constexpr auto operator<=>(const BodyHandle&) const = default;
	private:
		uint32_t m_Value = InvalidValue;
	};

	// How a body moves (Jolt's EMotionType). Scene/PhysicsSystem maps RigidBodyComponent::Type onto it; implicit static
	// bodies are Static, implicit sensor bodies and character inner bodies Kinematic (§5.3, §9.2, §9.6).
	enum class PhysicsMotionType : uint8_t
	{
		Static,
		Kinematic,
		Dynamic
	};

	// Discrete or continuous collision detection (Jolt's EMotionQuality; RigidBodyComponent::MotionQuality).
	enum class PhysicsMotionQuality : uint8_t
	{
		Discrete,
		LinearCast
	};

	// The degrees of freedom a dynamic body may move in (Jolt's EAllowedDOFs; RigidBodyComponent::LockTranslation and
	// LockRotation clear them per world axis). A Dynamic body needs at least one (PHYSICS_ALL_DOFS_LOCKED otherwise, §9.1);
	// other bodies always get All (BodyDescription::AllowedDofs).
	enum class PhysicsDofs : uint8_t
	{
		None = 0,
		TranslationX = 1 << 0,
		TranslationY = 1 << 1,
		TranslationZ = 1 << 2,
		RotationX = 1 << 3,
		RotationY = 1 << 4,
		RotationZ = 1 << 5,
		All = TranslationX | TranslationY | TranslationZ | RotationX | RotationY | RotationZ
	};

	template<>
	inline constexpr bool EnableFlagOperators<PhysicsDofs> = true;

	// A set of project physics layers (§9.2 "Queries take a layer mask"): bit i selects layer i of the project's
	// PhysicsSettings.Layers (PhysicsLayerTable). Bits at or above the table's layer count select nothing.
	using PhysicsLayerMask = uint32_t;

	// Every layer.
	inline constexpr PhysicsLayerMask AllPhysicsLayers = 0xffffffffu;

	// The largest coordinate magnitude, in metres, of anything physics places: a body's or character's position, a
	// collider's offset within its body, and the points of a hull or mesh (a collider's size has its own, smaller bound,
	// PhysicsShape::MaxColliderSize). Content beyond it (finite values the registry accepts) is refused
	// (PhysicsShape::Create, PhysicsWorld::CreateBody) or not applied (Scene/PhysicsSystem's teleports), so Jolt's broad
	// phase never sees bounds outside its range. Far beyond any game world (floats are 64 m apart there).
	inline constexpr float MaxPhysicsCoordinate = 1.0e9f;

	// The largest mass, in kilograms, of a Dynamic body or a character (PhysicsWorld::CreateBody and
	// CharacterController::Create refuse more; RigidBody.Mass and CharacterController.Mass have the same registry maximum):
	// with it, a body's inertia stays far inside the float range whatever its shape (Jolt asserts on an inertia it cannot
	// decompose), and so does the weight a character puts on what it stands on.
	inline constexpr float MaxPhysicsMass = 1.0e9f;

	// The largest magnitude, in m/s^2, of a component of a world's gravity (PhysicsWorld::Create and SetGravity refuse
	// more; the project setting PhysicsSettings.Gravity has the same bound): one step's velocity change from it, at the
	// largest gravity factor a body gets, stays far below the velocities whose squared length overflows a float.
	inline constexpr float MaxPhysicsGravity = 1.0e12f;

	// How far a rotation's squared length may be from 1 for physics to accept it as a unit quaternion (body poses, collider
	// rotations, character poses). Looser than Jolt's own Quat::IsNormalized (1e-5), so a rotation that went through a
	// matrix or a file is accepted; the Physics module normalizes it for Jolt.
	inline constexpr float PhysicsUnitRotationTolerance = 1.0e-3f;

	// A world pose: the position of a body's origin and its rotation (a unit quaternion).
	struct PhysicsPose
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity

		bool operator==(const PhysicsPose&) const = default;
	};

	// Whether every coordinate of `position` is finite and at most MaxPhysicsCoordinate in magnitude.
	[[nodiscard]] inline bool IsWithinPhysicsRange(const glm::vec3& position)
	{
		return std::abs(position.x) <= MaxPhysicsCoordinate && std::abs(position.y) <= MaxPhysicsCoordinate && std::abs(position.z) <= MaxPhysicsCoordinate;
	}

	// Whether `rotation` is finite with a squared length within PhysicsUnitRotationTolerance of 1.
	[[nodiscard]] inline bool IsPhysicsUnitRotation(const glm::quat& rotation)
	{
		if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
			return false;
		const float lengthSquared = rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z + rotation.w * rotation.w;
		return std::abs(lengthSquared - 1.0f) <= PhysicsUnitRotationTolerance;
	}

	// Whether physics places a body or a character at `pose`: its position within range (IsWithinPhysicsRange) and its
	// rotation a unit quaternion (IsPhysicsUnitRotation). PhysicsWorld::CreateBody, SetPose and MoveKinematic and
	// CharacterController::Create and SetPose take only such poses; Scene/PhysicsSystem checks every pose it computes
	// from the scene against this rule before it hands it over.
	[[nodiscard]] inline bool IsPlaceablePhysicsPose(const PhysicsPose& pose)
	{
		return IsWithinPhysicsRange(pose.Position) && IsPhysicsUnitRotation(pose.Rotation);
	}

	// The capacities of one PhysicsWorld (§9.1: PhysicsSystem::Init(maxBodies 16384, 0, maxBodyPairs 65536,
	// maxContactConstraints 16384) and a 16 MB TempAllocatorImpl). Approaching one (LimitWarningFraction of it) raises a
	// PHYSICS_LIMIT_EXCEEDED warning once per world; reaching the body limit refuses bodies with a PHYSICS_LIMIT_EXCEEDED
	// error, and a step whose body pairs or contact constraints overflow reports one (PhysicsWorld::Step). Tests lower them
	// to reach the limits quickly. Every member >= 1.
	struct PhysicsWorldLimits
	{
		static constexpr uint32_t DefaultMaxBodies = 16384;
		static constexpr uint32_t DefaultMaxBodyPairs = 65536;
		static constexpr uint32_t DefaultMaxContactConstraints = 16384;
		static constexpr size_t DefaultTempAllocatorBytes = size_t{ 16 } * 1024 * 1024;
		// The fraction of a limit at which the world warns that it is close (§9.1 "approaching a limit").
		static constexpr float LimitWarningFraction = 0.9f;

		uint32_t MaxBodies = DefaultMaxBodies;
		uint32_t MaxBodyPairs = DefaultMaxBodyPairs;
		uint32_t MaxContactConstraints = DefaultMaxContactConstraints;
		size_t TempAllocatorBytes = DefaultTempAllocatorBytes;
	};

	// "Static", "Kinematic" or "Dynamic".
	[[nodiscard]] constexpr std::string_view PhysicsMotionTypeToString(PhysicsMotionType type)
	{
		switch (type)
		{
			case PhysicsMotionType::Static:    return "Static";
			case PhysicsMotionType::Kinematic: return "Kinematic";
			case PhysicsMotionType::Dynamic:   return "Dynamic";
		}
		return "Unknown";
	}

}
