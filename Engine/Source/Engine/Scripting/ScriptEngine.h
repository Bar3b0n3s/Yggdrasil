#pragma once

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Scene/AnimationSystem.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scripting/Sandbox.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptTestHost.h"

#include <cstddef>
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
		// Optional safety clocks forwarded to Sandbox (SandboxSpecification::ClockSeconds and CpuClockSeconds: empty, lua_clock
		// and the main thread's CPU time, which the callback budget counts). Must be finite, nonnegative and monotonic, with
		// captures valid for the engine lifetime. Never used as simulation time.
		std::function<double()> ClockSeconds{};
		std::function<double()> CpuClockSeconds{};
		// M22 (C22.1, Docs/Decisions/0033-m22-contract.md decision 11): the locale Loc.Get reads string tables in and
		// Loc.GetLocale returns; empty reads every table in its fallback locale. M38 (Docs/Decisions/0055-m38-runtime-locale.md
		// decision 1): the VM's starting locale: a play session's VM starts in the session's locale (its start locale, or the
		// one a script set before a Scene.Load or Scene.Reload), the edit evaluation's in the project's Localization.Locale;
		// the session changes it with ScriptEngine::SetLocale.
		std::string Locale{};
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

	// M24 (C24.1, Docs/Decisions/0035-m24-contract.md decision 14; stream B): the instance member a graph behaviour's
	// transpiled text writes its live values into, a table keyed "<node>.<pin>" (AssetPipeline/Graph/GraphTranspiler.h). It
	// starts with "__", so no script field can take its name (IsReservedScriptFieldName).
	inline constexpr std::string_view ScriptLiveValuesKey = "__GraphLive";
	// The most entries ReadInstanceValues returns in each list; the rest are left out and Truncated is set.
	inline constexpr size_t MaxScriptInstanceValues = 16384;
	// The most rounds of OnStart one start flush runs (StartPending; ADR 0038 decision 14): an OnStart chain deeper than
	// this (each OnStart creating a scripted entity whose OnStart creates the next) is a runaway, reported as a script error
	// of each instance still waiting, which is disabled instead of started.
	inline constexpr uint32_t MaxScriptStartRounds = 16;

	// One value of an instance as ReadInstanceValues reads it.
	struct ScriptInstanceValue
	{
		std::string Key{};      // a member's name, or a live-value key
		std::string TypeName{}; // Luau's type name of the value ("number", "boolean", "string", "vector", "table") or the
								// engine type's ("Color", "Quat", "Entity", "AssetRef", a component proxy's type)
		// The value in reflection's JSON spelling (§6): a boolean, a finite number, a string, [x, y, z] for a vector,
		// [r, g, b, a] for a Color, [x, y, z, w] for a Quat, 16 hex digits for an Entity or an AssetRef; null for a value of
		// another type (a table, a function, a proxy), for a non-finite number and for a string that is not valid UTF-8, whose
		// TypeName still names it.
		VariantValue Value{};

		bool operator==(const ScriptInstanceValue&) const = default;
	};

	// One instance's values between frames, for the graph editor's live values (graph.liveValues).
	struct ScriptInstanceValues
	{
		std::vector<ScriptInstanceValue> Members{};    // the instance table's own string-keyed members, byte-wise by Key
		std::vector<ScriptInstanceValue> LiveValues{}; // the entries of its ScriptLiveValuesKey table, byte-wise by Key
		bool Truncated = false;                        // a list was cut at MaxScriptInstanceValues
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
	class ScriptEngine final : public IPhysicsEventListener, public IPhysics2DEventListener, public IAnimationEventListener
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

		// The start flush (§5.7 step 2, and the startup of a session or a scene transition; the frame phase runs none; ADR
		// 0038 decision 14), in rounds: each round synchronizes the instances (SynchronizeInstances), then the host's physics
		// bodies (IScriptHost::SynchronizePhysicsBodies), then snapshots the enabled, unstarted instances in (ExecutionOrder,
		// canonical entity) order and invokes OnStart exactly once on each, excluding pending destruction and fault-disabled
		// owners. Rounds repeat until no instance waits, so an instance created or enabled during the flush (by an OnStart,
		// or an OnCreate it runs) starts in the same flush, and when an entity first runs never depends on how ticks are
		// grouped into frames. A flush runs at most MaxScriptStartRounds rounds: each instance still waiting then is
		// published as a script error of its OnStart (a runaway chain) and disabled. In the fixed step's start flush, physics
		// callbacks held for started instances are then delivered (OnPhysicsEvent).
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
		// M21 (Docs/Decisions/0032-m21-contract.md decision 13, stream P2): the 2D world's events, delivered as OnPhysicsEvent
		// delivers the 3D ones (held for unstarted instances, every slot of the receiving entity in slot order), to the
		// callbacks OnCollisionEnter2D (with the contact), OnCollisionExit2D, OnTriggerEnter2D and OnTriggerExit2D; and its
		// diagnostics, turned into script errors as OnPhysicsDiagnostic turns the 3D ones.
		void OnPhysics2DEvent(const PhysicsEvent& event) override;
		void OnPhysics2DDiagnostic(const PhysicsDiagnostic& diagnostic) override;
		// M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 27; stream B): the UiEvents phase. Each event, in order,
		// bubbles by the class comment's rule from its target: the target, then each ancestor it had when its walk started,
		// in turn (so a callback that reparents an entity neither lengthens nor loops the walk); at each entity every
		// started, enabled instance whose behaviour defines the event's callback (GetUiEventCallbackName: OnUiClick,
		// OnUiValueChanged, OnUiFocus, OnUiHover, OnUiCancel, OnUiScroll, and C22.3's OnUiTextChanged and OnUiSubmit)
		// receives it in slot order, with the target as `element` and the event's value (OnUiValueChanged: a boolean for a
		// toggle, a number for a slider; OnUiFocus and OnUiHover: a boolean; OnUiScroll: a vector (x, y, 0); OnUiTextChanged
		// and OnUiSubmit: the target's UiTextInput Text as it is when the callback runs, empty without one); after an entity
		// where any of those callbacks returned the boolean true (no other value), the event goes no further. An event whose
		// target the scene no longer holds, or holds pending destruction, when its turn comes is dropped: nothing receives
		// it. During an event's walk, an ancestor that a callback of that event destroyed (so pending destruction when the
		// walk reaches it) is passed over, its instances receiving nothing, and the walk goes on to the next ancestor. A
		// callback's fault disables only its instance (§11.7), and the event goes on to the next receiver.
		void DispatchUiEvents(std::span<const UiEvent> events);
		// M23 (Docs/Decisions/0034-m23-contract.md decision 31): an AnimationSystem phase's event reaches
		// OnAnimationEvent(self, event) on every slot of the event's entity, in slot order (decision 17), as physics callbacks
		// do: a started instance at once, an unstarted one holding it (with its held physics callbacks, in arrival order)
		// until the first fixed-step start flush after its OnStart; destruction or a fault discards held events.
		void OnAnimationEvent(const AnimationEventRecord& event) override;
		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 2): Loc.SetLocale's checks and its OnLocaleChanged dispatch,
		// called by the binding around the host's change (IScriptHost::SetLocale), as Scene.CreateEntity calls NotifyCreated.
		// CheckLocaleChange: InvalidState while an OnLocaleChanged dispatch runs (no nested change) and once the engine stops
		// (a dispatch at teardown, from an OnDestroy of Stop); OK otherwise. NotifyLocaleChanged(previous), after the change,
		// with CheckLocaleChange's errors: delivers OnLocaleChanged(self, locale, previous) synchronously, before it returns
		// and under the calling callback's deadline, to every started, enabled instance (not pending destruction, not
		// fault-disabled) whose behaviour defines it, the caller's own included, in (ExecutionOrder, canonical entity, slot)
		// order from a snapshot taken before the first call; each is still eligible when its turn comes, and an instance
		// created during the dispatch (a new serial under its key included) receives nothing, its OnStart reading the new
		// locale. The world pause holds none (the UI's classification, M35's rule). A fault disables only its instance
		// (§11.7). A read-only engine dispatches nothing.
		[[nodiscard]] Status CheckLocaleChange() const;
		[[nodiscard]] Status NotifyLocaleChanged(std::string_view previous);
		// Idempotent. OnDestroy for all remaining instances and cancellation while host/scene/services still exist.
		void Stop();

		// `self` is the instance of the entity's slot whose behaviour's Script.Define name is `behaviour` (the first such
		// slot), or of its first slot when `behaviour` is empty; NotFound, before anything runs, when no slot of the entity
		// has that behaviour (its hint lists the entity's behaviours). A failed evaluation is the caller's error (ADR 0038,
		// friction F01): it is logged and returned, and `failure`, when given, receives its ScriptError (kind, location,
		// traceback, the bound entity and slot, the tick; bounded as a retained error is; empty after a success), but it is
		// never retained in the error stream, passed to the host (OnScriptError) or claimed by a test case. It is not a
		// callback of that instance either: the behaviour stays enabled, keeps its tasks and is not paused on (§11.7). A
		// failure that stopped the VM (a hard memory breach) is the game's and is published as ExecuteBytecode's are,
		// leaving `failure` empty. Spawned tasks belong to that instance, and their later faults are the game's. Its value
		// holds a vector, Color or Quat at any depth as the array of its components (Sandbox::Evaluate).
		[[nodiscard]] Result<ScriptEvaluation> Evaluate(std::string_view source, const VfsPath& path,
			std::optional<UUID> entity = std::nullopt, std::string_view behaviour = {}, std::optional<ScriptError>* failure = nullptr);
		// Trusted engine-produced replay predicates, including Dist. Never compiles source or creates a second VM. `self` is
		// the instance of the entity's first slot. A failure is published to the error stream, the host and the log (a
		// replay expectation's error is the game's), without disabling or pausing its instance. Its value stays strict JSON,
		// so a vector, Color or Quat fails the predicate as every value that is not JSON does (Sandbox::ExecuteBytecode;
		// ADR 0038's milestone review): a replay expectation in every player, test.run's and input.record's expectations
		// and play.waitFor's predicate fail loudly instead of holding.
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
		// M24 (C24.1, Docs/Decisions/0035-m24-contract.md decision 14; stream B): the values of the instance (entity, slot), its
		// own members and its live-value table's entries, read between phases for the editor's live values. It runs no Luau
		// and allocates nothing in the VM: raw reads only (no metamethod, so a class member behind __index is not listed),
		// iterating the tables as they are, pushing no new string, table or userdata, so neither the VM's heap nor its
		// collector's debt changes, every later result of the session is what it would have been without the read, and the
		// state hash, which instance tables are outside of anyway, is unchanged. A member that is not a string-keyed entry,
		// and a live-value table that is missing or not a table, contribute nothing. Main thread, never inside a protected
		// call (a callback, a task, an evaluation). Errors: NotFound when no instance is (entity, slot) (its hint names the
		// entity's behaviours); InvalidState after Stop and inside a protected call.
		[[nodiscard]] Result<ScriptInstanceValues> ReadInstanceValues(UUID entity, uint32_t slot) const;
		// Runs `operation`, host work that executes no Luau (Test.Screenshot's capture; Test.ReloadScript's reimport and type
		// check), with its time excluded from the callback budget of the protected call it runs in (§11.1, ADR 0022
		// "Integration"): the watchdog's wall deadline moves later by the time the operation took and the CPU time it used is
		// credited, both sampled from this engine's safety clocks. Luau that runs before or after it still counts, and so does
		// any protected call the operation makes after all (the excluded time ends there). Outside a protected call it only
		// runs the operation. Returns the operation's result. Main thread.
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
		// M22 (C22.1): the VM's locale, what Loc.Get reads and Loc.GetLocale returns: ScriptEngineSpecification::Locale until
		// the session sets another. M38 (decision 1): SetLocale, the session's (PlaySession::SetLocale; a test host's), takes
		// effect at the next Loc call and dispatches nothing (NotifyLocaleChanged does); `locale` is empty or a tag, which its
		// callers check.
		[[nodiscard]] std::string_view GetLocale() const;
		void SetLocale(std::string locale);
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
		// M38 (decision 1): the VM's locale, from m_Specification.Locale until SetLocale.
		std::string m_Locale{};
		friend struct Detail::ScriptEngineAccess;
	};

}
