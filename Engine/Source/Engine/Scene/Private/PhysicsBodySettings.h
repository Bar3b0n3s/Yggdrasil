#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/UUID.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/PhysicsComposition.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

// How the physics components map onto the physics vocabulary (Architecture §9.2 "Body settings"), and the small helpers
// the Scene module's physics files share: the composition (Scene/PhysicsComposition.cpp), the play session's PhysicsSystem
// (Scene/PhysicsSystem*.cpp) and the edit-time validation (Scene/PhysicsValidation.cpp). Private to Scene.

namespace Engine {

	namespace Utils {

		[[nodiscard]] inline bool IsFinite(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		[[nodiscard]] inline bool IsFinite(const glm::quat& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z) && std::isfinite(value.w);
		}

		// The collider index of a PhysicsShape::Create refusal, read from `reason`, the message after its physics code
		// ("collider <i>: <reason>"); nullopt when the message names no collider.
		[[nodiscard]] inline std::optional<uint32_t> ParseColliderIndex(std::string_view reason)
		{
			constexpr std::string_view Prefix = "collider ";
			if (!reason.starts_with(Prefix))
				return std::nullopt;
			const std::string_view digits = reason.substr(Prefix.size());
			uint32_t index = 0;
			const auto [end, error] = std::from_chars(digits.data(), digits.data() + digits.size(), index);
			if (error != std::errc() || end == digits.data() || end == digits.data() + digits.size() || *end != ':')
				return std::nullopt;
			return index;
		}

		// The RigidBody field a refusal of PhysicsShape::CheckDynamicBody is about: LockTranslation for PHYSICS_ALL_DOFS_LOCKED
		// (a body that cannot translate on a shape that cannot rotate), else Mass.
		[[nodiscard]] inline std::string_view GetDynamicBodyField(const Error& error)
		{
			return GetPhysicsDiagnosticCode(error) == PhysicsAllDofsLockedCode ? "LockTranslation" : "Mass";
		}

		// The component and field a refusal `error` of PhysicsShape::CheckDynamicBody with `massOverride` is about (M44): the
		// MassProperties' CenterOfMassOffset or InertiaScale when the body passes without its override (the offset's parallel-axis
		// inertia, or the scale, takes the inertia past the world's limit), with a hint to change it; else the RigidBody's
		// GetDynamicBodyField and no hint.
		struct DynamicBodyFault
		{
			std::string_view Component{};
			std::string_view Field{};
			std::string_view Hint{};
		};

		[[nodiscard]] inline DynamicBodyFault GetDynamicBodyFault(const PhysicsShape& shape, float mass, PhysicsDofs allowedDofs,
			const PhysicsMassOverride& massOverride, const Error& error)
		{
			if (massOverride != PhysicsMassOverride{} && shape.CheckDynamicBody(mass, allowedDofs).has_value())
			{
				const bool offsetFails = !shape.CheckDynamicBody(mass, allowedDofs, PhysicsMassOverride{ .CenterOfMassOffset = massOverride.CenterOfMassOffset }).has_value();
				return DynamicBodyFault{ .Component = "MassProperties",
					.Field = offsetFails ? "CenterOfMassOffset" : "InertiaScale",
					.Hint = offsetFails ? "bring the MassProperties' CenterOfMassOffset closer to the colliders; the play session does not create the body otherwise"
										: "lower the MassProperties' InertiaScale; the play session does not create the body otherwise" };
			}
			return DynamicBodyFault{ .Component = "RigidBody", .Field = GetDynamicBodyField(error) };
		}

		// The registry name of a collider type's component.
		[[nodiscard]] inline std::string_view GetColliderComponentName(PhysicsColliderType type)
		{
			switch (type)
			{
				case PhysicsColliderType::Box:     return "BoxCollider";
				case PhysicsColliderType::Sphere:  return "SphereCollider";
				case PhysicsColliderType::Capsule: return "CapsuleCollider";
				case PhysicsColliderType::Mesh:    return "MeshCollider";
				case PhysicsColliderType::Terrain: return "TerrainCollider";
			}

			ENGINE_CORE_ASSERT(false, "Unknown PhysicsColliderType {}", std::to_underlying(type));
			return "BoxCollider";
		}

		// M26 (Docs/Decisions/0037-m26-contract.md decision 21): whether `colliders` hold `entity`'s own TerrainCollider. A body
		// whose owner is `entity`, of origin ImplicitStatic, that holds it takes the TerrainCollider's Layer (the composition),
		// Friction and Restitution (the session) instead of layer 0 and the default material.
		[[nodiscard]] inline bool HoldsOwnTerrainCollider(std::span<const PhysicsColliderPlan> colliders, UUID entity)
		{
			return std::any_of(colliders.begin(), colliders.end(), [entity](const PhysicsColliderPlan& collider)
			{
				return collider.Type == PhysicsColliderType::Terrain && collider.Entity == entity;
			});
		}

		// RigidBody.Type -> PhysicsMotionType (§9.2: Type -> EMotionType).
		[[nodiscard]] inline PhysicsMotionType ToPhysicsMotionType(BodyType type)
		{
			switch (type)
			{
				case BodyType::Static:    return PhysicsMotionType::Static;
				case BodyType::Kinematic: return PhysicsMotionType::Kinematic;
				case BodyType::Dynamic:   return PhysicsMotionType::Dynamic;
			}

			ENGINE_CORE_ASSERT(false, "Unknown BodyType {}", std::to_underlying(type));
			return PhysicsMotionType::Static;
		}

		// RigidBody.MotionQuality -> PhysicsMotionQuality (§9.2: LinearCast -> CCD).
		[[nodiscard]] inline PhysicsMotionQuality ToPhysicsMotionQuality(MotionQuality quality)
		{
			return quality == MotionQuality::LinearCast ? PhysicsMotionQuality::LinearCast : PhysicsMotionQuality::Discrete;
		}

		// The degrees of freedom RigidBody.LockTranslation and LockRotation leave, per world axis (§9.2: lock flags ->
		// EAllowedDOFs).
		[[nodiscard]] inline PhysicsDofs GetRigidBodyDofs(const RigidBodyComponent& body)
		{
			PhysicsDofs dofs = PhysicsDofs::None;
			if (!body.LockTranslation.x)
				dofs |= PhysicsDofs::TranslationX;
			if (!body.LockTranslation.y)
				dofs |= PhysicsDofs::TranslationY;
			if (!body.LockTranslation.z)
				dofs |= PhysicsDofs::TranslationZ;
			if (!body.LockRotation.x)
				dofs |= PhysicsDofs::RotationX;
			if (!body.LockRotation.y)
				dofs |= PhysicsDofs::RotationY;
			if (!body.LockRotation.z)
				dofs |= PhysicsDofs::RotationZ;
			return dofs;
		}

	}

}
