#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ParticleEmitterComponent.h"
#include "Engine/Scene/Components/ParticleStateComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <cstdint>
#include <format>

// The ParticleEmitter proxy's methods (M26, C-PART; Docs/Decisions/0037-m26-contract.md decision 26, stream P-A):
// ParticleEmitter:Emit and Clear, which write only the emitter's runtime particle state (Scene/ParticleSystem.h's
// QueueParticleEmit and ClearParticles), shown at the next Particles phase, and GetParticleCount, which reads it. Particles are
// presentation: no simulated state changes, so neither the scene's revision nor the state hash moves and no external mutation
// is reported. Emit and Clear still count as writes, so a read-only evaluation (an edit scene's) refuses them and edit scenes
// never hold particles. The state advances in the frame phase only, so no simulation-path code may read it (W26.1's review,
// decision 26): GetParticleCount raises in a gameplay callback of the fixed step.

namespace Engine {

	namespace Utils {

		// The receiver of a particle method: a ParticleEmitter on a live entity (ValidateComponent refuses one pending
		// destruction), or for a read one that is still readable (ValidateComponentForRead; M20, W-P02).
		static Entity ParticleSelf(ScriptCall& call, bool read)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			const Status valid = read ? ScriptProxy::ValidateComponentForRead(host, proxy) : ScriptProxy::ValidateComponent(host, proxy);
			if (!valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "ParticleEmitter")
				Lua::RaiseError(call, "wrong component receiver");
			return read ? host.GetScene().FindReadableEntityByID(proxy.Entity.ID) : host.GetScene().FindEntityByID(proxy.Entity.ID);
		}

		// ParticleEmitter:Emit(count): count particles queued for the next Particles phase (QueueParticleEmit).
		static int ParticleEmit(ScriptCall& call)
		{
			const Entity entity = ParticleSelf(call, false);
			const uint32_t count = Lua::Check<uint32_t>(call, 2);
			if (const Status queued = QueueParticleEmit(entity, count); !queued)
				return Lua::RaiseError(call, queued.error());
			return 0;
		}

		// ParticleEmitter:Clear(): every live particle killed and the queued emits dropped (ClearParticles).
		static int ParticleClear(ScriptCall& call)
		{
			const Entity entity = ParticleSelf(call, false);
			if (const Status cleared = ClearParticles(entity); !cleared)
				return Lua::RaiseError(call, cleared.error());
			return 0;
		}

		// ParticleEmitter:GetParticleCount(): the live particles after the last Particles phase, 0 without a state. The Particles
		// phase steps the particles in the frame phase, so the count a fixed step would see depends on how frames grouped the
		// ticks (the second of two ticks run in one frame still sees the count from before the first), and a fixed step that
		// branched on it would make the state hash depend on frame pacing: gameplay code of the fixed step (ScriptFrameState::Phase
		// Step: OnFixedUpdate, OnStart at the step's start flush, physics, animation and UI events, the tasks resumed there) is
		// refused. The frame phase's callbacks (OnUpdate, OnLateUpdate), test-suite code and evaluations read it.
		static int ParticleGetCount(ScriptCall& call)
		{
			const Entity entity = ParticleSelf(call, true);
			const bool gameplay = call.Execution != nullptr && call.Execution->Origin == ScriptExecutionOrigin::Gameplay;
			if (gameplay && call.Engine->GetHost().GetFrameState().Phase == InputPhase::Step)
			{
				return Lua::RaiseError(call, "particles are presentation state that the frame phase advances, so a fixed-step callback (OnFixedUpdate, a "
											 "task, a physics, animation or UI event) cannot read their count; read it in OnUpdate or OnLateUpdate");
			}
			const ParticleStateComponent* state = entity.TryGetComponent<ParticleStateComponent>();
			Lua::Push(call, static_cast<uint32_t>(state == nullptr ? 0 : state->State.Particles.size()));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterParticles(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions write{};
			api.Type("ParticleEmitter")
				.Method("Emit", Utils::ParticleEmit, "(self: ParticleEmitter, count: number) -> ()",
					std::format("Queue count particles (1 to {}) for the next Particles phase, whether the emitter is Playing, looping or finished; "
								"an emitter without particles yet starts at that phase. A count outside its range raises an error. Particles are "
								"presentation, so the simulated state does not change.",
						MaxParticlesPerEmitter),
					write)
				.Method("Clear", Utils::ParticleClear, "(self: ParticleEmitter) -> ()",
					"Kill every live particle and drop the queued emits; the emission cycle and the random stream go on.", write)
				.Method("GetParticleCount", Utils::ParticleGetCount, "(self: ParticleEmitter) -> number",
					"Read the live particles after the last Particles phase (0 before the first). Particles are presentation state the frame phase "
					"advances, so the count depends on frame pacing: it raises an error in a fixed-step callback (OnFixedUpdate, a task, a "
					"physics, animation or UI event) and is read in OnUpdate, OnLateUpdate or a test case; never let it drive gameplay.",
					read);
			return {};
		}

	}

}
