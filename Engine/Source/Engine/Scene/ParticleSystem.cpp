#include "EnginePCH.h"
#include "Engine/Scene/ParticleSystem.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSimulation.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <entt/entity/registry.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

// The Particles phase's system (ParticleSystem.h; M26, stream P-A, Docs/Decisions/0037-m26-contract.md decisions 5 and 6).
// Everything it writes is the runtime ParticleStateComponent, which no simulation phase reads and nothing hashes or
// serializes, so the phase never changes a state hash.

namespace Engine {

	namespace Utils {

		static uint32_t SaturatingAdd(uint32_t value, uint64_t increment)
		{
			const uint64_t sum = static_cast<uint64_t>(value) + std::min<uint64_t>(increment, std::numeric_limits<uint32_t>::max());
			return static_cast<uint32_t>(std::min<uint64_t>(sum, std::numeric_limits<uint32_t>::max()));
		}

		// The emitter's world matrix this frame: the frame's transform update has just written WorldTransformComponent; an
		// entity without one (a scene no transform update has run on) computes it.
		static glm::mat4 GetEmitterWorld(Entity entity)
		{
			if (const WorldTransformComponent* world = entity.TryGetComponent<WorldTransformComponent>())
				return world->Matrix;
			return TransformSystem::ComputeWorldMatrix(entity);
		}

		// A ParticleEmitter entity's state, made seeded and unanchored when it has none.
		static ParticleStateComponent& EnsureState(Entity entity, const ParticleEmitterComponent& emitter)
		{
			if (ParticleStateComponent* state = entity.TryGetComponent<ParticleStateComponent>())
				return *state;
			return entity.AddComponent<ParticleStateComponent>(
				ParticleStateComponent{ .State = ParticleEmitterState(MakeParticleEmitterSeed(entity.GetUUID(), emitter.Seed)), .LastTick = std::nullopt });
		}

		// Ends the states of entities that lost their ParticleEmitter or became effectively disabled (their particles end at
		// once). An entity pending destruction keeps its components until the flush destroys it.
		static void EndOrphanedStates(Scene& scene)
		{
			entt::registry& registry = scene.GetRegistry();
			std::vector<entt::entity> ended;
			for (const entt::entity handle : registry.view<ParticleStateComponent>())
			{
				if (!registry.all_of<ParticleEmitterComponent>(handle) || registry.all_of<HierarchyDisabledTag>(handle))
					ended.push_back(handle);
			}
			for (const entt::entity handle : ended)
			{
				const Entity entity(handle, &scene);
				if (entity.IsValid())
					entity.RemoveComponent<ParticleStateComponent>();
			}
		}

	}

	ParticleSystem::ParticleSystem(float fixedDelta)
		: m_FixedDelta(fixedDelta)
	{
		ENGINE_CORE_ASSERT(std::isfinite(fixedDelta) && fixedDelta > 0.0f, "A particle step of {} s", fixedDelta);
	}

	void ParticleSystem::Step(Scene& scene, uint64_t tick)
	{
		m_Stats = {};
		entt::registry& registry = scene.GetRegistry();
		if (!registry.view<ParticleStateComponent>().empty())
			Utils::EndOrphanedStates(scene);
		// A scene without emitters (every scene before M26) costs two empty views.
		if (registry.view<ParticleEmitterComponent>().empty())
			return;

		// The effectively enabled emitters, gathered first so the states added below never run while the canonical order is
		// being read.
		m_Emitters.clear();
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const Entity entity = scene.FindEntityByID(id);
			if (entity.IsValid() && entity.HasComponent<ParticleEmitterComponent>() && !entity.HasComponent<HierarchyDisabledTag>())
				m_Emitters.push_back(id);
		}

		for (const UUID id : m_Emitters)
		{
			const Entity entity = scene.FindEntityByID(id);
			const ParticleEmitterComponent& emitter = entity.GetComponent<ParticleEmitterComponent>();
			ParticleStateComponent& state = Utils::EnsureState(entity, emitter);
			// A state no phase has stepped starts here: one step at this phase (none at tick 0), never the ticks before it.
			if (!state.LastTick.has_value())
				state.LastTick = tick > 0 ? tick - 1 : 0;
			const uint64_t due = tick > *state.LastTick ? tick - *state.LastTick : 0;
			const uint64_t steps = std::min(due, MaxParticleStepsPerPhase);
			if (steps > 0)
			{
				const glm::mat4 world = Utils::GetEmitterWorld(entity);
				const uint32_t cappedBefore = state.State.Capped;
				for (uint64_t step = 0; step < steps; ++step)
					StepParticleEmitter(emitter, world, m_FixedDelta, state.State);
				m_Stats.Steps = Utils::SaturatingAdd(m_Stats.Steps, steps);
				m_Stats.Capped = Utils::SaturatingAdd(m_Stats.Capped, state.State.Capped - cappedBefore);
			}
			state.LastTick = tick;
			m_Stats.Emitters = Utils::SaturatingAdd(m_Stats.Emitters, 1);
			m_Stats.Particles = Utils::SaturatingAdd(m_Stats.Particles, state.State.Particles.size());
		}
	}

	Status QueueParticleEmit(Entity entity, uint32_t count)
	{
		if (count < 1 || count > MaxParticlesPerEmitter)
			return MakeError(ErrorCode::InvalidArgument, "ParticleEmitter:Emit needs a count from 1 to {}, not {}", MaxParticlesPerEmitter, count);
		if (!entity.IsValid())
			return MakeError(ErrorCode::NotFound, "the entity whose particles to emit no longer exists");
		const ParticleEmitterComponent* emitter = entity.TryGetComponent<ParticleEmitterComponent>();
		if (emitter == nullptr)
			return MakeError(ErrorCode::NotFound, "the entity '{}' holds no ParticleEmitter", entity.GetName());
		ParticleStateComponent& state = Utils::EnsureState(entity, *emitter);
		state.State.PendingEmits = Utils::SaturatingAdd(state.State.PendingEmits, count);
		return {};
	}

	Status ClearParticles(Entity entity)
	{
		if (!entity.IsValid())
			return MakeError(ErrorCode::NotFound, "the entity whose particles to clear no longer exists");
		if (!entity.HasComponent<ParticleEmitterComponent>())
			return MakeError(ErrorCode::NotFound, "the entity '{}' holds no ParticleEmitter", entity.GetName());
		if (ParticleStateComponent* state = entity.TryGetComponent<ParticleStateComponent>())
		{
			state->State.Particles.clear();
			state->State.PendingEmits = 0;
		}
		return {};
	}

	std::optional<uint64_t> FindParticleDigest(const Entity& entity)
	{
		if (!entity.IsReadable() || !entity.HasComponent<ParticleEmitterComponent>())
			return std::nullopt;
		const ParticleStateComponent* state = entity.TryGetComponent<ParticleStateComponent>();
		if (state == nullptr || !state->LastTick.has_value())
			return std::nullopt;
		return ComputeParticleDigest(state->State);
	}

}
