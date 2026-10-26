#pragma once

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class ScriptApiRegistry;
	class TaskScheduler;

	namespace Detail {

		struct ScriptEngineAccess;

	}

	struct ScriptEngineSpecification
	{
		// Required borrowed back-references, outlive the engine. Host's scene incarnation is fixed for this VM. Api is
		// frozen against Host.GetTypes; its test counters are main-thread-only and shared across a test run's VMs.
		IScriptHost* Host = nullptr;
		ScriptApiRegistry* Api = nullptr;
		ScriptingSettings Settings{};
		RunModes Mode = RunModes::Editor; // exactly one of Editor, Release, Dist
		bool TestMode = false;
		// Optional test-mode host installed before OnCreate/OnStart, for setup/teardown errors and Debug.Break counts.
		// Borrowed for the entire engine lifetime; CollectSuite must use this same host when it is supplied.
		IScriptTestHost* TestHost = nullptr;
		// Edit-context eval owns its own VM and borrowed edit-scene host. All host writes, including random-stream
		// advancement, are rejected before a side effect. Local Color/Quat/tables/local Random values remain mutable.
		bool ReadOnly = false;
		// Optional safety clock forwarded to Sandbox; empty uses lua_clock. Must be finite, nonnegative and
		// monotonic, with captures valid for the engine lifetime. Never used as simulation time.
		std::function<double()> ClockSeconds{};
	};

	// One behaviour instance. M20 (S-MULTI; Docs/Decisions/0031-m20-contract.md decision 17): an instance is identified by
	// (Entity, Slot), Slot being the index of its script in the entity's Script list.
	struct ScriptInstanceInfo
	{
		UUID Entity{};
		AssetHandle Script{};
		bool Started = false;
		bool Disabled = false;
		int32_t ExecutionOrder = 0;
		uint32_t Slot = 0;
	};

	struct ScriptReloadResult
	{
		bool Deferred = false;
		std::vector<AssetHandle> Scripts{}; // dependency order: required modules first, then dependent behaviours
	};

	// One runtime-scene VM and its instances, tasks, require cache and error stream (§11). Main-thread-only, including
	// creation/destruction. No Lua state escapes; references are checked against this VM's unique generation. The host
	// consumes quit/scene-load/fatal-stop requests only after protected calls unwind. Simulate creates no ScriptEngine.
	//
	// Script slots (M20, S-MULTI; Docs/Decisions/0031-m20-contract.md decisions 17 and 18; stream M-A). The Script component
	// is an ordered list of scripts, and every slot whose script is set is its own instance, keyed (entity UUID, slot), with
	// its own table, field overrides, ExecutionOrder, tasks, held physics events and fault state. Wherever this class says
	// "(ExecutionOrder, canonical entity) order" the order is (ExecutionOrder, canonical entity, slot). A fault disables
	// only its own instance; the entity's other slots keep running. Entity-addressed callbacks (OnEnable, OnDisable, the
	// physics callbacks) reach every started slot of the entity in slot order, each slot holding or discarding its own
	// events; OnDestroy runs for the slots of one entity in reverse slot order, children first as before. A write of the
	// Script component (its list) keeps an instance whose slot index still holds the same script asset and destroys and
	// creates the others, as removing and adding the component would. Evaluate and ExecuteBytecode address an entity's
	// first slot, or the slot whose behaviour is named by the caller (script.eval's `script`).
	//
	// Bubbling is defined per behaviour (decision 18), for the bubbling callbacks later milestones add (M22's UI events; no
	// callback bubbles before them): a bubbling event visits its target entity, then each ancestor; at each entity every
	// behaviour that defines the callback receives it, in slot order; propagation to the next ancestor stops after an
	// entity where any behaviour's callback returned true, and that entity's other behaviours still receive it.
	// Non-bubbling callbacks ignore return values, so no slot can keep another from receiving one.
	class ScriptEngine final : public IPhysicsEventListener
	{
	public:
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ScriptEngine;
		};

		explicit ScriptEngine(ConstructionKey key, const ScriptEngineSpecification& specification);
		~ScriptEngine() override;
		ScriptEngine(const ScriptEngine&) = delete;
		ScriptEngine& operator=(const ScriptEngine&) = delete;

		// Creates and seals one Sandbox, but does not invoke gameplay. InvalidArgument for absent services/settings;
		// InvalidState before process Luau startup or with an unfrozen/mismatched registry. Startup callers create
		// physics first, then InitializeInstances and StartPending, then release PlayOnStart audio (§5.6).
		[[nodiscard]] static Result<Scope<ScriptEngine>> Create(const ScriptEngineSpecification& specification);
		// Makes all instance tables before running OnCreate in (ExecutionOrder, canonical entity) order. Individual
		// script faults publish ScriptError and disable only their owner. Infrastructure failures return an Error.
		// ReadOnly engines build resolved field tables/class links only, without lifecycle callbacks or task execution.
		[[nodiscard]] Status InitializeInstances();
		// Detects added/changed Script components and creates missing instances before the next phase snapshot.
		// Re-enabled instances are recreated; fault-disabled instances stay disabled until successful explicit reload.
		[[nodiscard]] Status SynchronizeInstances();
		// Immediate Scene.CreateEntity/Instantiate hook, before returning to the script. Validates the entire entity
		// list before creating instances. Nested OnCreate inherits the outer deadline; recursion beyond 16 is Script.
		[[nodiscard]] Status NotifyCreated(std::span<const UUID> entities);

		// Snapshot enabled, unstarted instances in (ExecutionOrder, canonical entity) order; invoke OnStart exactly once. Exclude
		// pending destruction and fault-disabled owners. An instance created during this snapshot starts next phase.
		// In the fixed step's start flush, physics callbacks held for started instances are then delivered (OnPhysicsEvent).
		void StartPending();
		// The session invokes these only in §5.7's matching phases. Each snapshots eligible started instances, excludes
		// pending-destroy/disabled/unstarted entries, and never visits an entity created later in that phase. Host frame
		// state is the sole source of delta/tick/time/phase. A failure disables its instance and does not abort peers.
		void FixedUpdate();
		void Update();
		void LateUpdate();
		void ResumeTasks();
		// Runs OnDestroy and cancels owned tasks for newly destroyed/disabled instances, children first. Marks each
		// before invoking user code to prevent recursive duplicate teardown. Session alternates this with physics
		// exits until no new destruction remains, then removes voices/instances/entities. Return: number handled.
		[[nodiscard]] uint32_t PrepareDestroyFlush();
		void FinishDestroyFlush();
		// Started instances receive the callback at once. A created instance that has not started yet holds the event, in
		// dispatch order, until the first fixed-step start flush after its OnStart, so physics callbacks stay in the fixed
		// step and enter/exit pairs stay symmetric (§9.4); destruction or a fault discards held events.
		void OnPhysicsEvent(const PhysicsEvent& event) override;
		void OnPhysicsDiagnostic(const PhysicsDiagnostic& diagnostic) override;
		// Idempotent. OnDestroy for all remaining instances and cancellation while host/scene/services still exist.
		void Stop();

		// `self` is the instance of the entity's slot whose behaviour's Script.Define name is `behaviour` (the first such
		// slot), or of its first slot when `behaviour` is empty; NotFound, before anything runs, when no slot of the entity
		// has that behaviour (its hint lists the entity's behaviours). A failed evaluation is published to the error stream
		// and log, but it is not a callback of that instance: the behaviour stays enabled, keeps its tasks and is not
		// paused on (§11.7). Spawned tasks belong to that instance.
		[[nodiscard]] Result<ScriptEvaluation> Evaluate(std::string_view source, const VfsPath& path,
			std::optional<UUID> entity = std::nullopt, std::string_view behaviour = {});
		// Trusted engine-produced replay predicates, including Dist. Never compiles source or creates a second VM. `self` is
		// the instance of the entity's first slot. Failures follow Evaluate's reporting policy.
		[[nodiscard]] Result<ScriptEvaluation> ExecuteBytecode(const ScriptData& script,
			std::optional<UUID> entity = std::nullopt);
		// EditorOnly: resolve the complete dependency closure, execute candidates in isolation, then atomically patch
		// tables in place and add new default fields. Failure rolls back the whole chain. Reloads defer when the host
		// owns deterministic time, except an explicit synchronous Test.ReloadScript in a currently active test suite.
		// Surviving instances receive OnHotReload. An instance without a table (module-top or memory fault) is recreated
		// from defaults and overrides and restarts its lifecycle: OnCreate now, OnStart at the next start flush.
		[[nodiscard]] Result<ScriptReloadResult> Reload(AssetHandle script, bool fromTest = false);
		// EditorOnly: the scripts Reload(script) would re-execute now, in its dependency order: the script itself, then the
		// loaded modules and attached behaviours that transitively require it. Unloaded project scripts that require it
		// (for example test suites) are not part of the chain. Executes no script code and publishes nothing.
		[[nodiscard]] Result<std::vector<AssetHandle>> GetReloadChain(AssetHandle script);
		// Runs `operation`, host work that executes no Luau (Test.Screenshot's capture; Test.ReloadScript's reimport and type
		// check), with its wall time excluded from the callback budget of the protected call it runs in (§11.1, ADR 0022
		// "Integration"): the watchdog's deadline moves later by the time the operation took, sampled from this engine's
		// safety clock. Luau that runs before or after it still counts, and so does any protected call the operation makes
		// after all (the excluded time ends there). Outside a protected call it only runs the operation. Returns the
		// operation's result. Main thread.
		[[nodiscard]] Status RunHostOperation(const std::function<Status()>& operation);

		// A suite owns the installed test-host lifetime. A second collection before EndSuite is InvalidState. Executes
		// the suite body only, under one protected entry; returns case functions in declaration order. Failure releases
		// every partial case reference and restores the specification's host. Only TestMode accepts this API.
		[[nodiscard]] Result<ScriptCollectedSuite> CollectSuite(AssetHandle script, IScriptTestHost& host);
		// Creates a sandboxed coroutine without executing it. Only one case is active. ResumeCase is called after
		// normal tasks in phase 4; waits not due return Yielded without execution. Every actual resume gets a new
		// deadline. A VM fault is an owned ScriptCaseResume.Error; invalid/foreign references are outer Result errors.
		[[nodiscard]] Result<ScriptReference> StartCase(ScriptReference caseFunction);
		[[nodiscard]] Result<ScriptCaseResume> ResumeCase(ScriptReference caseThread);
		// Idempotent, including stale references. Cancels waits, case-owned tasks and audio capture. ReleaseReference
		// also releases filtered-out function references. EndSuite cancels all suite work and restores the specification's
		// host (or null), so setup/teardown reporting can still reach a host supplied for the engine's entire lifetime.
		void CancelCase(ScriptReference caseThread);
		void ReleaseReference(ScriptReference reference);
		void EndSuite();

		[[nodiscard]] IScriptHost& GetHost() const;
		[[nodiscard]] ScriptApiRegistry& GetApi() const;
		[[nodiscard]] const ScriptingSettings& GetSettings() const;
		[[nodiscard]] RunModes GetRunMode() const;
		[[nodiscard]] bool IsTestMode() const;
		[[nodiscard]] bool IsReadOnly() const;
		[[nodiscard]] bool IsStopped() const;
		[[nodiscard]] uint64_t GetGeneration() const;
		[[nodiscard]] ScriptMemoryState GetMemoryState() const;
		[[nodiscard]] std::vector<ScriptInstanceInfo> GetInstances() const;
		[[nodiscard]] ScriptErrorStream& GetErrors();
		[[nodiscard]] const ScriptErrorStream& GetErrors() const;
		// Null before Create succeeds or after Stop. Non-owning; lifetime never exceeds this engine.
		[[nodiscard]] TaskScheduler* GetTaskScheduler();
		[[nodiscard]] IScriptTestHost* GetTestHost() const;
	private:
		struct State;
		Scope<State> m_State{};
		ScriptEngineSpecification m_Specification{};
		ScriptErrorStream m_Errors{};
		friend struct Detail::ScriptEngineAccess;
	};

}
