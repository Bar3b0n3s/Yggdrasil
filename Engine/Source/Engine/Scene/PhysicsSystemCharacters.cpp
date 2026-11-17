#include "EnginePCH.h"
#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Physics/CharacterController.h"
#include "Engine/Scene/Private/PhysicsBodySettings.h"
#include "Engine/Scene/Private/PhysicsSystemState.h"

#include <cmath>

// The script-facing character functions of the play session (Architecture §9.6; §11.5 CharacterController:Move,
// IsGrounded, GetGroundNormal, GetVelocity, and M36's GetGroundCollider, IsSqueezed and GetSqueezingBody). The characters'
// lifecycle (creation in canonical order among the bodies, rebuilds, teleports, updates and write-back) belongs to the step
// phases in PhysicsSystem.cpp.

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

	PhysicsCharacterState PhysicsSystem::State::MakeCharacterState(const CharacterController& controller) const
	{
		const CharacterGroundState ground = controller.GetGroundState();
		PhysicsCharacterState state{ .IsGrounded = ground.IsGrounded, .GroundNormal = ground.GroundNormal, .Velocity = controller.GetVelocity() };
		// M36: the collider entity of the ground, resolved as a query hit on it is (a character's inner body names the
		// character; a disabled or destroyed ground names none).
		if (ground.IsGrounded && ground.GroundBody.IsValid())
		{
			if (const std::optional<PhysicsOverlapHit> hit = ResolveHit(ground.GroundBody, ground.GroundCollider))
				state.GroundCollider = hit->Entity;
		}
		// M36 W36.2: the squeeze, and the owner of the body that squeezes. Never a character's inner body: it is placed, never
		// moved at a velocity of its own, so it pushes nothing (CharacterController.h, the squeeze's pushers).
		const CharacterSqueeze squeeze = controller.GetSqueeze();
		state.IsSqueezed = squeeze.IsSqueezed;
		if (squeeze.IsSqueezed)
		{
			// The body may be gone since the update (FindBody verifies what an index names; this handle is the controller's).
			if (const auto body = Bodies.find(squeeze.Body.GetValue()); body != Bodies.end() && IsPresent(body->second.Owner))
				state.SqueezedBy = body->second.Owner;
		}
		return state;
	}

	Result<PhysicsCharacterState> PhysicsSystem::GetCharacterState(UUID entity) const
	{
		const auto found = m_State->Characters.find(entity);
		if (found == m_State->Characters.end() || !m_State->IsPresent(entity))
			return MakeError(ErrorCode::NotFound, "entity {} has no character", entity);
		return m_State->MakeCharacterState(*found->second.Controller);
	}

}
