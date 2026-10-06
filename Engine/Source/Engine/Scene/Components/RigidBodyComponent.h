#pragma once

#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <string>

namespace Engine {

	// Registry enum "BodyType" (Architecture §9.2).
	enum class BodyType : uint8_t
	{
		Static,
		Kinematic,
		Dynamic
	};

	// Registry enum "MotionQuality" (Architecture §9.2): LinearCast is Jolt's continuous collision detection.
	enum class MotionQuality : uint8_t
	{
		Discrete,
		LinearCast
	};

	// Registry name "RigidBody" (Architecture §5.3, §9.2): simulates the entity (and its collider-only descendants, as one
	// compound) with Jolt. Requires Transform; excludes CharacterController. Mass in kilograms (Dynamic only, > 0);
	// Friction and Restitution live on the body because Jolt stores them per body; Layer names a project physics layer;
	// MaxAngularVelocity in rad/s (Jolt's default 0.25 * pi * 60); EnhancedInternalEdgeRemoval is Jolt's
	// mEnhancedInternalEdgeRemoval, set on rolling bodies (§9.2); initial velocities apply when the body is created. The
	// MotionQuality member shares its enum's name, so the type is qualified with the namespace.
	struct RigidBodyComponent
	{
		BodyType Type = BodyType::Dynamic;
		float Mass = 1.0f;
		float Friction = 0.5f;
		float Restitution = 0.0f;
		float LinearDamping = 0.05f;
		float AngularDamping = 0.05f;
		float GravityFactor = 1.0f;
		Engine::MotionQuality MotionQuality = Engine::MotionQuality::Discrete;
		bool AllowSleeping = true;
		glm::bvec3 LockTranslation = glm::bvec3(false);
		glm::bvec3 LockRotation = glm::bvec3(false);
		std::string Layer = "Default";
		float MaxLinearVelocity = 500.0f;
		float MaxAngularVelocity = 47.12f;
		bool EnhancedInternalEdgeRemoval = false;
		glm::vec3 InitialLinearVelocity = glm::vec3(0.0f);
		glm::vec3 InitialAngularVelocity = glm::vec3(0.0f);
	};

}
