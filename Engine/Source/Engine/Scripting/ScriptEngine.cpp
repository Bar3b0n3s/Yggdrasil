#include "EnginePCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/IDComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"
#include "Engine/Scripting/Private/ScriptSlots.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		struct ScriptEvaluationOwnerScope
		{
			ScriptTaskOwner& Owner;
			ScriptTaskOwner Previous{};

			ScriptEvaluationOwnerScope(ScriptTaskOwner& owner, ScriptTaskOwner next)
				: Owner(owner), Previous(owner)
			{
				Owner = next;
			}
			~ScriptEvaluationOwnerScope() { Owner = Previous; }
		};

	}

	ScriptEngine::ScriptEngine(ConstructionKey /*key*/, const ScriptEngineSpecification& specification)
		: m_Specification(specification)
	{
		m_State = CreateScope<State>(*this);
		m_State->TestHost = specification.TestHost;
	}

	ScriptEngine::~ScriptEngine()
	{
		Stop();
		// State teardown needs the specification and error stream, which otherwise die before the first member.
		m_State.reset();
	}

	Result<Scope<ScriptEngine>> ScriptEngine::Create(const ScriptEngineSpecification& specification)
	{
		if (specification.Host == nullptr || specification.Api == nullptr || specification.Settings.MemoryLimitMB == 0
			|| specification.Settings.CallbackBudgetMs == 0 || specification.Host->GetSceneGeneration() == 0
			|| (specification.Mode != RunModes::Editor && specification.Mode != RunModes::Release && specification.Mode != RunModes::Dist)
			|| (specification.TestHost != nullptr && !specification.TestMode))
			return MakeError(ErrorCode::InvalidArgument, "ScriptEngine needs a host, registry, valid limits and one run mode");
		if (!Detail::IsScriptingRuntimeInitialized() || !specification.Api->IsFrozen())
			return MakeError(ErrorCode::InvalidState, "ScriptEngine needs process scripting startup and a frozen API registry");
		ENGINE_TRY(specification.Api->Freeze(specification.Host->GetTypes()));
		auto engine = CreateScope<ScriptEngine>(ConstructionKey{}, specification);
		SandboxSpecification vm{};
		vm.Mode = SandboxMode::Runtime;
		vm.MemoryLimitMB = specification.Settings.MemoryLimitMB;
		vm.CallbackBudgetMs = specification.Settings.CallbackBudgetMs;
		vm.IsTestRun = specification.TestMode;
		vm.ReadOnly = specification.ReadOnly;
		vm.ClockSeconds = specification.ClockSeconds;
		vm.Host = specification.Host;
		vm.Engine = engine.get();
		vm.Api = specification.Api;
		vm.RandomStream = &specification.Host->GetRandom();
		vm.CookedModules = [host = specification.Host](const VfsPath& path) -> Result<Ref<const ScriptData>>
		{
			AssetManager* assets = host->GetAssets();
			if (assets == nullptr)
				return MakeError(ErrorCode::NotFound, "No asset manager supplies required scripts");
			const auto handle = assets->Resolve(path.GetScheme() == "project" ? path.GetPath() : path.ToString());
			if (!handle)
				return MakeError(ErrorCode::NotFound, "Required script '{}' is not registered", path.ToString());
			ENGINE_TRY_ASSIGN(auto asset, assets->Load(*handle));
			auto script = AssetCast<ScriptData>(asset);
			if (script == nullptr)
				return MakeError(ErrorCode::Validation, "The required asset '{}' is not a script", path.ToString());
			return script;
		};
		ENGINE_TRY_ASSIGN(auto sandbox, Sandbox::Create(vm));
		engine->m_State->Vm = std::move(sandbox);
		engine->m_State->Scheduler = CreateScope<TaskScheduler>(*engine);
		return engine;
	}

	Status ScriptEngine::InitializeInstances()
	{
		m_State->AssertOwner();
		if (m_State->Initialized || IsStopped())
			return MakeError(ErrorCode::InvalidState, "Script instances are already initialized or the engine stopped");
		m_State->Initialized = true;
		return m_State->CreateInstances(m_State->MissingInstances(GetHost().GetScene().GetCanonicalOrder()));
	}

	Status ScriptEngine::SynchronizeInstances()
	{
		m_State->AssertOwner();
		if (!m_State->Initialized || IsStopped())
			return MakeError(ErrorCode::InvalidState, "Script instances are not running");
		Scene& scene = GetHost().GetScene();
		m_State->RefreshLifecycleOrder();
		// Only entities with a Script component and existing instances can change, so only those are visited, in canonical
		// order. The refreshed lifecycle order already lists every instance canonically; a scene walk is needed only when
		// a scripted entity may need a new instance. Large scenes with few scripts therefore cost no per-entity work.
		using InstanceKey = State::InstanceKey;
		const auto mayCreate = [this](Entity entity, InstanceKey key)
		{
			return entity.IsActive() || !m_State->DisabledHistory.contains(key);
		};
		entt::registry& registry = scene.GetRegistry();
		std::vector<UUID> unplaced;
		for (const entt::entity handle : registry.view<const ScriptComponent>())
		{
			const UUID id = registry.get<const IDComponent>(handle).ID;
			const ScriptComponent& component = registry.get<const ScriptComponent>(handle);
			for (uint32_t slot = 0; slot < Detail::GetScriptSlotCount(component); ++slot)
			{
				const InstanceKey key{ id, slot };
				if (!Detail::GetScriptSlot(component, slot).Script.IsValid() || m_State->Instances.contains(key))
					continue;
				const Entity entity = scene.FindEntityByID(id);
				if (!entity)
					break;
				if (mayCreate(entity, key))
				{
					unplaced.push_back(id);
					break;
				}
			}
		}
		std::vector<UUID> snapshot;
		if (unplaced.empty())
		{
			// The refreshed lifecycle order keeps an entity's slots together; removed instances in it are skipped below.
			for (const InstanceKey key : m_State->LifecycleOrder)
			{
				if (snapshot.empty() || snapshot.back() != key.Entity)
					snapshot.push_back(key.Entity);
			}
		}
		else
		{
			std::vector<UUID> candidates = m_State->SortedInstanceIDs();
			candidates.insert(candidates.end(), unplaced.begin(), unplaced.end());
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			for (const UUID id : scene.GetCanonicalOrder())
			{
				if (std::binary_search(candidates.begin(), candidates.end(), id))
					snapshot.push_back(id);
			}
		}
		struct ActiveTransition
		{
			InstanceKey Key{};
			bool Active = false;
			int32_t Order = 0;
			int Table = LUA_NOREF;
		};
		std::vector<InstanceKey> missing;
		std::vector<InstanceKey> enabled;
		std::vector<ActiveTransition> transitions;
		for (const UUID id : snapshot)
		{
			Entity entity = scene.FindEntityByID(id);
			if (!entity)
				continue;
			// A write of the Script list keeps an instance whose slot still holds the same script; the others are destroyed,
			// in reverse slot order as a removed component's, and recreated below (decision 17's list writes).
			const std::vector<InstanceKey> existing = m_State->InstancesOf(id);
			for (auto current = existing.rbegin(); current != existing.rend(); ++current)
			{
				auto found = m_State->Instances.find(*current);
				// A disable already observed owns a teardown even if the entity was enabled again before the flush.
				// Its replacement receives OnCreate/OnEnable after FinishDestroyFlush removes the old instance.
				if (found == m_State->Instances.end() || found->second.DestroyPrepared || m_State->DisabledHistory.contains(*current))
					continue;
				// An earlier slot's OnDestroy may have destroyed the entity: its remaining instances belong to the flush.
				entity = scene.FindEntityByID(id);
				if (!entity)
					break;
				const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
				const AssetHandle handle = component != nullptr && current->Slot < Detail::GetScriptSlotCount(*component)
					? Detail::GetScriptSlot(*component, current->Slot).Script
					: AssetHandle{};
				if (found->second.Handle == handle)
					continue;
				found->second.DestroyPrepared = true;
				if (!IsReadOnly())
					m_State->Invoke(*current, "OnDestroy", nullptr, true);
				m_State->Scheduler->CancelInstance(current->Entity, current->Slot);
				found = m_State->Instances.find(*current);
				if (found != m_State->Instances.end())
					m_State->EraseInstance(found);
			}
			// OnDestroy may have destroyed the entity or changed its component.
			entity = scene.FindEntityByID(id);
			const ScriptComponent* component = entity ? entity.TryGetComponent<ScriptComponent>() : nullptr;
			if (component == nullptr)
				continue;
			const bool active = entity.IsActive();
			for (uint32_t slot = 0; slot < Detail::GetScriptSlotCount(*component); ++slot)
			{
				const InstanceKey key{ id, slot };
				const auto found = m_State->Instances.find(key);
				if (found != m_State->Instances.end() && (found->second.DestroyPrepared || m_State->DisabledHistory.contains(key)))
					continue;
				const Detail::ScriptSlotView view = Detail::GetScriptSlot(*component, slot);
				if (found == m_State->Instances.end())
				{
					if (view.Script.IsValid() && mayCreate(entity, key))
					{
						missing.push_back(key);
						if (m_State->DisabledHistory.contains(key))
							enabled.push_back(key);
					}
					continue;
				}
				found->second.Order = view.ExecutionOrder;
				found->second.Name = entity.GetName();
				if (found->second.Active == active)
					continue;
				found->second.Active = active;
				if (!active)
					m_State->DisabledHistory.insert(key);
				transitions.push_back({ key, active, found->second.Order, found->second.Table });
			}
		}
		// Observe the entire hierarchy before callbacks can change it again. A parent's OnDisable may
		// re-enable its subtree; every descendant must still complete the disable already observed.
		// OnEnable and OnDisable are addressed to an entity: they reach its instances together, in slot order, at the turn
		// of its slot that comes first in (ExecutionOrder, canonical entity) order (decision 17), which for one slot per
		// entity is (ExecutionOrder, canonical entity) order. `transitions` lists an entity's slots together, in canonical
		// entity order and slot order.
		struct TransitionGroup
		{
			size_t Begin = 0;
			size_t End = 0;
			int32_t Order = 0; // the lowest ExecutionOrder of the group's slots
		};
		std::vector<TransitionGroup> groups;
		for (size_t index = 0; index < transitions.size(); ++index)
		{
			if (groups.empty() || transitions[groups.back().Begin].Key.Entity != transitions[index].Key.Entity)
				groups.push_back({ index, index, transitions[index].Order });
			groups.back().End = index + 1;
			groups.back().Order = std::min(groups.back().Order, transitions[index].Order);
		}
		std::stable_sort(groups.begin(), groups.end(), [](const TransitionGroup& left, const TransitionGroup& right)
		{
			return left.Order < right.Order;
		});
		if (!IsReadOnly())
		{
			for (const TransitionGroup& group : groups)
			{
				for (size_t index = group.Begin; index < group.End; ++index)
				{
					const ActiveTransition& transition = transitions[index];
					const auto current = m_State->Instances.find(transition.Key);
					if (current != m_State->Instances.end() && current->second.Table == transition.Table && !current->second.DestroyPrepared)
						m_State->Invoke(transition.Key, transition.Active ? "OnEnable" : "OnDisable", nullptr, true);
				}
			}
		}
		// OnCreate callbacks a faulted slice deferred run first, as members of the earlier batch they belong to.
		m_State->InvokeDeferredCreates();
		ENGINE_TRY(m_State->CreateInstances(missing));
		for (const InstanceKey key : enabled)
		{
			m_State->DisabledHistory.erase(key);
			if (!IsReadOnly())
				m_State->Invoke(key, "OnEnable");
		}
		return {};
	}

	Status ScriptEngine::NotifyCreated(std::span<const UUID> entities)
	{
		m_State->AssertOwner();
		if (IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		for (const UUID id : entities)
		{
			if (!GetHost().GetScene().FindEntityByID(id))
				return MakeError(ErrorCode::NotFound, "A newly created script entity no longer exists");
		}
		if (m_State->CreationDepth >= 16)
			return MakeError(ErrorCode::Script, "Nested script creation exceeds the maximum depth of 16");
		struct DepthScope
		{
			uint32_t& Depth;
			explicit DepthScope(uint32_t& depth)
				: Depth(depth)
			{
				++Depth;
			}
			~DepthScope() { --Depth; }
		} depth(m_State->CreationDepth);
		return m_State->CreateInstances(m_State->MissingInstances(entities));
	}

	void ScriptEngine::StartPending()
	{
		m_State->AssertOwner();
		if (IsStopped() || IsReadOnly())
			return;
		const Status synchronized = SynchronizeInstances();
		if (!synchronized)
		{
			m_State->Publish(synchronized.error());
			return;
		}
		const auto snapshot = m_State->OrderedInstances(false);
		for (const State::InstanceKey key : snapshot)
		{
			const auto found = m_State->Instances.find(key);
			if (found == m_State->Instances.end() || found->second.Started || found->second.FaultDisabled || found->second.DestroyPrepared)
				continue;
			const Entity entity = GetHost().GetScene().FindEntityByID(key.Entity);
			if (!entity || !entity.IsActive())
				continue;
			found->second.Started = true;
			m_State->Invoke(key, "OnStart");
		}
		// Physics callbacks belong to the fixed step. Contacts that began before an instance's OnStart follow the whole
		// start flush of the next fixed step (frame-rate independent), before FixedUpdate, so each side sees its enter
		// before the matching exit (§9.4).
		if (GetHost().GetFrameState().Phase == InputPhase::Step)
			m_State->DeliverPendingPhysics();
	}

	void ScriptEngine::FixedUpdate()
	{
		m_State->Phase("OnFixedUpdate");
	}
	void ScriptEngine::Update()
	{
		m_State->Phase("OnUpdate");
	}
	void ScriptEngine::LateUpdate()
	{
		m_State->Phase("OnLateUpdate");
	}

	void ScriptEngine::ResumeTasks()
	{
		m_State->AssertOwner();
		if (!IsStopped() && !IsReadOnly())
			m_State->Scheduler->ResumeDue();
	}

	uint32_t ScriptEngine::PrepareDestroyFlush()
	{
		m_State->AssertOwner();
		if (m_State->Initialized && !IsStopped())
		{
			const Status synchronized = SynchronizeInstances();
			if (!synchronized)
				m_State->Publish(synchronized.error());
		}
		m_State->RefreshLifecycleOrder();
		// Reverse canonical entity order and reverse slot order: children first, and an entity's slots last to first.
		const std::vector<State::InstanceKey> snapshot = m_State->LifecycleOrder;
		uint32_t handled = 0;
		for (auto current = snapshot.rbegin(); current != snapshot.rend(); ++current)
		{
			const auto found = m_State->Instances.find(*current);
			if (found == m_State->Instances.end() || found->second.DestroyPrepared)
				continue;
			const Entity entity = GetHost().GetScene().FindEntityByID(current->Entity);
			if (entity && !m_State->DisabledHistory.contains(*current))
				continue;
			found->second.DestroyPrepared = true;
			++handled;
			if (!IsReadOnly() && !IsStopped())
				m_State->Invoke(*current, "OnDestroy", nullptr, true);
			m_State->Scheduler->CancelInstance(current->Entity, current->Slot);
		}
		return handled;
	}

	void ScriptEngine::FinishDestroyFlush()
	{
		m_State->AssertOwner();
		for (auto current = m_State->Instances.begin(); current != m_State->Instances.end();)
		{
			if (!current->second.DestroyPrepared)
			{
				++current;
				continue;
			}
			// A script fault cannot be cured by toggling Active. Preserve its disabled record until explicit reload.
			if (current->second.FaultDisabled && GetHost().GetScene().FindEntityByID(current->first.Entity))
			{
				current->second.DestroyPrepared = false;
				m_State->DisabledHistory.erase(current->first);
				++current;
				continue;
			}
			if (!GetHost().GetScene().FindEntityByID(current->first.Entity))
				m_State->DisabledHistory.erase(current->first);
			current = m_State->EraseInstance(current);
		}
	}

	void ScriptEngine::OnPhysicsEvent(const PhysicsEvent& event)
	{
		m_State->AssertOwner();
		// Every instance of the receiving entity, in slot order; each holds or discards its own events. A callback may
		// remove a later slot's instance, which then receives nothing.
		for (const State::InstanceKey key : m_State->InstancesOf(event.Self))
		{
			const auto found = m_State->Instances.find(key);
			if (found == m_State->Instances.end())
				continue;
			if (!found->second.Started || !found->second.PendingPhysics.empty())
			{
				// Physics marks this side entered whether or not a script observes it, so dropping the event would leave a
				// later exit unpaired. Hold it (behind any earlier held event) until the instance has started; a removed or
				// faulted instance discards its queue.
				if (!IsStopped() && !IsReadOnly() && !found->second.FaultDisabled && !found->second.DestroyPrepared && found->second.Table != LUA_NOREF)
					found->second.PendingPhysics.push_back(event);
				continue;
			}
			m_State->InvokePhysics(key, event);
		}
	}

	void ScriptEngine::OnPhysicsDiagnostic(const PhysicsDiagnostic& diagnostic)
	{
		m_State->AssertOwner();
		if (diagnostic.Severity != DiagnosticSeverity::Error)
			return;
		ScriptError error{};
		error.Kind = ScriptErrorKind::Runtime;
		error.Message = diagnostic.Code + ": " + diagnostic.Message;
		error.Callback = "Physics";
		error.Entity = diagnostic.Entity;
		// Physics already logged this diagnostic; retain script observability and policy without a second log entry. The
		// diagnostic belongs to the entity, not to one of its behaviours, so it names, and disables, the entity's first slot
		// that has an instance (a slot without a script has none), as an entity-addressed evaluation does; an entity without
		// an instance is named from the scene.
		const std::vector<State::InstanceKey> instances = m_State->InstancesOf(diagnostic.Entity);
		if (instances.empty())
		{
			if (const Entity entity = GetHost().GetScene().FindEntityByID(diagnostic.Entity))
				error.EntityName = entity.GetName();
		}
		const uint32_t slot = instances.empty() ? 0 : instances.front().Slot;
		m_State->Publish(std::move(error), { .Entity = diagnostic.Entity, .Case = m_State->CurrentOwner.Case, .Slot = slot }, false, true);
	}

	void ScriptEngine::Stop()
	{
		if (m_State == nullptr || m_State->Vm == nullptr || m_State->Stopping)
			return;
		m_State->AssertOwner();
		m_State->Stopping = true;
		m_State->RefreshLifecycleOrder();
		const std::vector<State::InstanceKey> snapshot = m_State->LifecycleOrder;
		for (auto current = snapshot.rbegin(); current != snapshot.rend(); ++current)
		{
			const auto found = m_State->Instances.find(*current);
			if (found == m_State->Instances.end() || found->second.DestroyPrepared)
				continue;
			found->second.DestroyPrepared = true;
			if (!IsReadOnly() && !IsStopped())
				m_State->Invoke(*current, "OnDestroy", nullptr, true);
		}
		m_State->Scheduler->CancelAll();
		EndSuite();
		m_State->Stopped = true;
	}

	Result<ScriptEvaluation> ScriptEngine::Evaluate(std::string_view source, const VfsPath& path, std::optional<UUID> entity,
		std::string_view behaviour)
	{
		m_State->AssertOwner();
		if (IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		uint32_t slot = 0;
		if (entity.has_value() && !behaviour.empty())
		{
			const std::vector<State::InstanceKey> instances = m_State->InstancesOf(*entity);
			const auto named = std::ranges::find_if(instances, [this, behaviour](State::InstanceKey key)
			{
				const State::Instance& instance = m_State->Instances.find(key)->second;
				return instance.Script != nullptr && instance.Script->Name == behaviour;
			});
			if (named == instances.end())
			{
				std::string names;
				for (const State::InstanceKey key : instances)
				{
					const State::Instance& instance = m_State->Instances.find(key)->second;
					if (instance.Script != nullptr)
						names += std::format("{}{} (slot {})", names.empty() ? "" : ", ", instance.Script->Name, key.Slot);
				}
				return std::unexpected(Error(ErrorCode::NotFound, std::format("entity {} has no script slot whose behaviour is named '{}'", *entity, behaviour))
						.WithHint(names.empty() ? std::string("the entity has no behaviour instance") : "its behaviours: " + names));
			}
			slot = named->Slot;
		}
		// Tasks the evaluation spawns belong to its instance, and to a case only when evaluated from that case's work. The
		// owner's slot is also the instance `self` binds to (ScriptEngineAccess::PushInstance).
		Utils::ScriptEvaluationOwnerScope owner(m_State->CurrentOwner, { entity.value_or(UUID{}), m_State->CurrentOwner.Case, slot });
		auto result = m_State->Vm->Evaluate(source, path, entity);
		if (!result)
		{
			const auto failure = m_State->Vm->GetLastError();
			if (failure)
				m_State->Publish(*failure, { .Entity = entity.value_or(UUID{}), .Case = m_State->ActiveCase, .Slot = slot }, false, false, State::FaultPolicy::ReportOnly);
		}
		return result;
	}

	Result<ScriptEvaluation> ScriptEngine::ExecuteBytecode(const ScriptData& script, std::optional<UUID> entity)
	{
		m_State->AssertOwner();
		if (IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		Utils::ScriptEvaluationOwnerScope owner(m_State->CurrentOwner, { entity.value_or(UUID{}), m_State->CurrentOwner.Case });
		auto result = m_State->Vm->ExecuteBytecode(script, entity);
		if (!result)
		{
			const auto failure = m_State->Vm->GetLastError();
			if (failure)
				m_State->Publish(*failure, { .Entity = entity.value_or(UUID{}), .Case = m_State->ActiveCase }, false, false, State::FaultPolicy::ReportOnly);
		}
		return result;
	}

	IScriptHost& ScriptEngine::GetHost() const
	{
		return *m_Specification.Host;
	}
	ScriptApiRegistry& ScriptEngine::GetApi() const
	{
		return *m_Specification.Api;
	}
	const ScriptingSettings& ScriptEngine::GetSettings() const
	{
		return m_Specification.Settings;
	}
	RunModes ScriptEngine::GetRunMode() const
	{
		return m_Specification.Mode;
	}
	bool ScriptEngine::IsTestMode() const
	{
		return m_Specification.TestMode;
	}
	Status ScriptEngine::RunHostOperation(const std::function<Status()>& operation)
	{
		ENGINE_CORE_ASSERT(operation, "RunHostOperation needs an operation");
		ENGINE_CORE_ASSERT(m_State->Vm != nullptr, "the engine's Sandbox exists from Create on");
		Detail::SandboxAccess::BeginHostTime(*m_State->Vm);
		Status result = operation();
		Detail::SandboxAccess::EndHostTime(*m_State->Vm);
		return result;
	}
	bool ScriptEngine::IsReadOnly() const
	{
		return m_Specification.ReadOnly;
	}
	bool ScriptEngine::IsStopped() const
	{
		return m_State->Stopped || m_State->Vm == nullptr || m_State->Vm->IsStopped();
	}
	uint64_t ScriptEngine::GetGeneration() const
	{
		return m_State->Vm != nullptr ? Detail::SandboxAccess::GetGeneration(*m_State->Vm) : 0;
	}
	ScriptMemoryState ScriptEngine::GetMemoryState() const
	{
		return m_State->Vm != nullptr ? m_State->Vm->GetMemoryState() : ScriptMemoryState{};
	}

	std::vector<ScriptInstanceInfo> ScriptEngine::GetInstances() const
	{
		m_State->AssertOwner();
		std::vector<ScriptInstanceInfo> result;
		for (const UUID id : GetHost().GetScene().GetCanonicalOrder())
		{
			for (const State::InstanceKey key : m_State->InstancesOf(id))
			{
				const State::Instance& instance = m_State->Instances.find(key)->second;
				result.push_back({ id, instance.Handle, instance.Started, instance.FaultDisabled || !instance.Active || instance.DestroyPrepared,
					instance.Order, key.Slot });
			}
		}
		return result;
	}

	ScriptErrorStream& ScriptEngine::GetErrors()
	{
		return m_Errors;
	}
	const ScriptErrorStream& ScriptEngine::GetErrors() const
	{
		return m_Errors;
	}
	TaskScheduler* ScriptEngine::GetTaskScheduler()
	{
		return IsStopped() ? nullptr : m_State->Scheduler.get();
	}
	IScriptTestHost* ScriptEngine::GetTestHost() const
	{
		return m_State->TestHost;
	}

}
