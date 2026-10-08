#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/CharacterController.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"

#include <cmath>

// The script-facing character functions of the play session (Architecture §9.6; §11.5 CharacterController:Move,
// IsGrounded, GetGroundNormal, GetVelocity). The characters' lifecycle (creation in canonical order among the bodies,
// rebuilds, teleports, updates and write-back) belongs to the step phases in PhysicsSystem.cpp.

namespace Engine {

	UUID PhysicsSystem::State::FindCharacterOwner(BodyHandle body) const
	{
		const auto found = InnerBodies.find(body.GetValue());
		return found != InnerBodies.end() ? found->second : UUID();
	}

	Status PhysicsSystem::MoveCharacter(UUID entity, const glm::vec3& velocity)
	{
		const auto found = m_State->Characters.find(entity);
		if (found == m_State->Characters.end() || !m_State->IsPresent(entity))
			return MakeError(ErrorCode::NotFound, "entity {} has no character", entity);
		if (!Utils::IsFinite(velocity))
			return MakeError(ErrorCode::InvalidArgument, "the velocity ({}, {}, {}) is not finite", velocity.x, velocity.y, velocity.z);
		if (std::abs(velocity.x) > MaxCharacterSpeed || std::abs(velocity.y) > MaxCharacterSpeed || std::abs(velocity.z) > MaxCharacterSpeed)
		{
			return MakeError(ErrorCode::InvalidArgument, "the velocity ({}, {}, {}) is out of range: each component must be at most {} m/s", velocity.x,
				velocity.y, velocity.z, MaxCharacterSpeed);
		}
		found->second.DesiredVelocity = velocity;
		return {};
	}

	Result<PhysicsCharacterState> PhysicsSystem::GetCharacterState(UUID entity) const
	{
		const auto found = m_State->Characters.find(entity);
		if (found == m_State->Characters.end() || !m_State->IsPresent(entity))
			return MakeError(ErrorCode::NotFound, "entity {} has no character", entity);
		const CharacterController& controller = *found->second.Controller;
		const CharacterGroundState ground = controller.GetGroundState();
		return PhysicsCharacterState{ .IsGrounded = ground.IsGrounded, .GroundNormal = ground.GroundNormal, .Velocity = controller.GetVelocity() };
	}

}
