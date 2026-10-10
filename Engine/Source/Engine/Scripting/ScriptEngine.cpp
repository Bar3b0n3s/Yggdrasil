#include "EnginePCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>

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
		const auto order = GetHost().GetScene().GetCanonicalOrder();
		const std::vector<UUID> snapshot(order.begin(), order.end());
		return m_State->CreateInstances(snapshot);
	}

	Status ScriptEngine::SynchronizeInstances()
	{
		m_State->AssertOwner();
		if (!m_State->Initialized || IsStopped())
			return MakeError(ErrorCode::InvalidState, "Script instances are not running");
		Scene& scene = GetHost().GetScene();
		m_State->RefreshLifecycleOrder();
		const auto order = scene.GetCanonicalOrder();
		const std::vector<UUID> snapshot(order.begin(), order.end());
		struct ActiveTransition
		{
			UUID Entity{};
			bool Active = false;
			int32_t Order = 0;
			int Table = LUA_NOREF;
		};
		std::vector<UUID> missing;
		std::vector<UUID> enabled;
		std::vector<ActiveTransition> transitions;
		for (const UUID id : snapshot)
		{
			const Entity entity = scene.FindEntityByID(id);
			if (!entity)
				continue;
			const ScriptComponent* component = entity.TryGetComponent<ScriptComponent>();
			const AssetHandle handle = component != nullptr ? component->Script.GetHandle() : AssetHandle{};
			auto found = m_State->Instances.find(id);
			// A disable already observed owns a teardown even if the entity was enabled again before the flush.
			// Its replacement receives OnCreate/OnEnable after FinishDestroyFlush removes the old instance.
			if (found != m_State->Instances.end() && (found->second.DestroyPrepared || m_State->DisabledHistory.contains(id)))
				continue;
			if (found != m_State->Instances.end() && found->second.Handle != handle)
			{
				found->second.DestroyPrepared = true;
				if (!IsReadOnly())
					m_State->Invoke(id, "OnDestroy", nullptr, true);
				m_State->Scheduler->CancelEntity(id);
				found = m_State->Instances.find(id);
				if (found != m_State->Instances.end())
				{
					if (found->second.Table != LUA_NOREF)
						lua_unref(Detail::SandboxAccess::MainCall(*m_State->Vm).State, found->second.Table);
					m_State->Instances.erase(found);
				}
				found = m_State->Instances.end();
			}
			if (found == m_State->Instances.end())
			{
				if (handle.IsValid() && (entity.IsActive() || !m_State->DisabledHistory.contains(id)))
				{
					missing.push_back(id);
					if (m_State->DisabledHistory.contains(id))
						enabled.push_back(id);
				}
				continue;
			}
			if (component == nullptr)
				continue;
			found->second.Order = component->ExecutionOrder;
			found->second.Name = entity.GetName();
			const bool active = entity.IsActive();
			if (found->second.Active == active)
				continue;
			found->second.Active = active;
			if (!active)
				m_State->DisabledHistory.insert(id);
			transitions.push_back({ id, active, found->second.Order, found->second.Table });
		}
		// Observe the entire hierarchy before callbacks can change it again. A parent's OnDisable may
		// re-enable its subtree; every descendant must still complete the disable already observed.
		std::stable_sort(transitions.begin(), transitions.end(), [](const ActiveTransition& left, const ActiveTransition& right)
		{
			return left.Order < right.Order;
		});
		if (!IsReadOnly())
		{
			for (const auto& transition : transitions)
			{
				const auto current = m_State->Instances.find(transition.Entity);
				if (current != m_State->Instances.end() && current->second.Table == transition.Table && !current->second.DestroyPrepared)
					m_State->Invoke(transition.Entity, transition.Active ? "OnEnable" : "OnDisable", nullptr, true);
			}
		}
		ENGINE_TRY(m_State->CreateInstances(missing));
		for (const UUID id : enabled)
		{
			m_State->DisabledHistory.erase(id);
			if (!IsReadOnly())
				m_State->Invoke(id, "OnEnable");
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
		return m_State->CreateInstances(entities);
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
		for (const UUID id : snapshot)
		{
			const auto found = m_State->Instances.find(id);
			if (found == m_State->Instances.end() || found->second.Started || found->second.FaultDisabled || found->second.DestroyPrepared)
				continue;
			const Entity entity = GetHost().GetScene().FindEntityByID(id);
			if (!entity || !entity.IsActive())
				continue;
			found->second.Started = true;
			m_State->Invoke(id, "OnStart");
		}
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
		const std::vector<UUID> snapshot = m_State->LifecycleOrder;
		uint32_t handled = 0;
		for (auto current = snapshot.rbegin(); current != snapshot.rend(); ++current)
		{
			const auto found = m_State->Instances.find(*current);
			if (found == m_State->Instances.end() || found->second.DestroyPrepared)
				continue;
			const Entity entity = GetHost().GetScene().FindEntityByID(*current);
			if (entity && !m_State->DisabledHistory.contains(*current))
				continue;
			found->second.DestroyPrepared = true;
			++handled;
			if (!IsReadOnly() && !IsStopped())
				m_State->Invoke(*current, "OnDestroy", nullptr, true);
			m_State->Scheduler->CancelEntity(*current);
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
			if (current->second.FaultDisabled && GetHost().GetScene().FindEntityByID(current->first))
			{
				current->second.DestroyPrepared = false;
				m_State->DisabledHistory.erase(current->first);
				++current;
				continue;
			}
			if (current->second.Table != LUA_NOREF)
				lua_unref(Detail::SandboxAccess::MainCall(*m_State->Vm).State, current->second.Table);
			if (!GetHost().GetScene().FindEntityByID(current->first))
				m_State->DisabledHistory.erase(current->first);
			current = m_State->Instances.erase(current);
		}
	}

	void ScriptEngine::OnPhysicsEvent(const PhysicsEvent& event)
	{
		m_State->AssertOwner();
		const auto found = m_State->Instances.find(event.Self);
		if (found == m_State->Instances.end() || !found->second.Started)
			return;
		switch (event.Type)
		{
			case PhysicsEventType::CollisionEnter: m_State->Invoke(event.Self, "OnCollisionEnter", &event); return;
			case PhysicsEventType::CollisionExit:  m_State->Invoke(event.Self, "OnCollisionExit", &event); return;
			case PhysicsEventType::TriggerEnter:   m_State->Invoke(event.Self, "OnTriggerEnter", &event); return;
			case PhysicsEventType::TriggerExit:    m_State->Invoke(event.Self, "OnTriggerExit", &event); return;
		}
		ENGINE_CORE_ASSERT(false, "Unknown physics event");
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
		// Physics already logged this diagnostic; retain script observability and policy without a second log entry.
		m_State->Publish(std::move(error), { .Entity = diagnostic.Entity, .Case = m_State->CurrentOwner.Case }, false, true);
	}

	void ScriptEngine::Stop()
	{
		if (m_State == nullptr || m_State->Vm == nullptr || m_State->Stopping)
			return;
		m_State->AssertOwner();
		m_State->Stopping = true;
		m_State->RefreshLifecycleOrder();
		const std::vector<UUID> snapshot = m_State->LifecycleOrder;
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

	Result<ScriptEvaluation> ScriptEngine::Evaluate(std::string_view source, const VfsPath& path, std::optional<UUID> entity)
	{
		m_State->AssertOwner();
		if (IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		Utils::ScriptEvaluationOwnerScope owner(m_State->CurrentOwner,
			{ entity.value_or(UUID{}), !m_State->CurrentOwner.Case.IsNull() ? m_State->CurrentOwner.Case : m_State->ActiveCase });
		auto result = m_State->Vm->Evaluate(source, path, entity);
		if (!result)
		{
			const auto failure = m_State->Vm->GetLastError();
			if (failure)
				m_State->Publish(*failure, { .Entity = entity.value_or(UUID{}), .Case = m_State->ActiveCase });
		}
		return result;
	}

	Result<ScriptEvaluation> ScriptEngine::ExecuteBytecode(const ScriptData& script, std::optional<UUID> entity)
	{
		m_State->AssertOwner();
		if (IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		Utils::ScriptEvaluationOwnerScope owner(m_State->CurrentOwner,
			{ entity.value_or(UUID{}), !m_State->CurrentOwner.Case.IsNull() ? m_State->CurrentOwner.Case : m_State->ActiveCase });
		auto result = m_State->Vm->ExecuteBytecode(script, entity);
		if (!result)
		{
			const auto failure = m_State->Vm->GetLastError();
			if (failure)
				m_State->Publish(*failure, { .Entity = entity.value_or(UUID{}), .Case = m_State->ActiveCase });
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
			const auto found = m_State->Instances.find(id);
			if (found != m_State->Instances.end())
				result.push_back({ id, found->second.Handle, found->second.Started,
					found->second.FaultDisabled || !found->second.Active || found->second.DestroyPrepared, found->second.Order });
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
