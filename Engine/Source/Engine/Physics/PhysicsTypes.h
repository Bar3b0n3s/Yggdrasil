#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

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

	// A world pose: the position of a body's origin and its rotation (a unit quaternion).
	struct PhysicsPose
	{
		glm::vec3 Position = glm::vec3(0.0f);
		glm::quat Rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity

		bool operator==(const PhysicsPose&) const = default;
	};

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
