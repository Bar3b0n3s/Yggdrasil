#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"

#include <glm/glm.hpp>

#include <string>

namespace Engine {

	// Registry name "RigidBody2D" (Architecture §5.3; M21, Docs/Decisions/0032-m21-contract.md decision 9): simulates the entity
	// (and its 2D collider-only descendants, as one compound) in the play session's 2D world (Scene/Physics2DSystem.h), a
	// second Jolt world whose Dynamic bodies move only along world X and Y and rotate only about world Z (Physics2DDofs,
	// Physics/Physics2DWorld.h). Requires Transform. The members mean what RigidBody's do (BodyType and MotionQuality are its
	// registry enums), in the plane: FixedRotation keeps a Dynamic body from rotating (Physics2DTranslationDofs);
	// InitialLinearVelocity is in metres per second along world X and Y and InitialAngularVelocity in radians per second
	// about world Z (counter-clockwise seen from +Z), both applied when the body is created. Layer names a project physics
	// layer: the 2D world uses the 3D world's layers and collision matrix (PhysicsSettings). EnhancedInternalEdgeRemoval is
	// RigidBody's (Jolt's mEnhancedInternalEdgeRemoval, §9.2 "Seams"): set it on a body that rolls or slides across the
	// merged boxes of one compound (C21.2's tilemap collider), so it crosses their seams without ghost contacts. An entity
	// that also holds a 3D physics component is PHYSICS2D_MIXED_WORLDS: it stays in the 3D world and its 2D components make
	// no body.
	struct RigidBody2DComponent
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
		bool FixedRotation = false;
		std::string Layer = "Default";
		float MaxLinearVelocity = 500.0f;
		float MaxAngularVelocity = 47.12f;
		bool EnhancedInternalEdgeRemoval = false;
		glm::vec2 InitialLinearVelocity = glm::vec2(0.0f);
		float InitialAngularVelocity = 0.0f;
	};

}
