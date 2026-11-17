#pragma once

#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/TaskScheduler.h"

#include <lua.h>

#include <compare>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <variant>
#include <vector>

namespace Engine {

	namespace Detail {

		// A behaviour instance's identity (Docs/Decisions/0031-m20-contract.md decision 17): its entity and the index of its
		// script in the entity's Script list. Ordered by entity, then slot.
		struct ScriptInstanceKey
		{
			UUID Entity{};
			uint32_t Slot = 0;

			constexpr auto operator<=>(const ScriptInstanceKey&) const = default;
		};

		// A physics callback held for an instance that has not started: the event and the world that raised it (M21,
		// Docs/Decisions/0032-m21-contract.md decision 13: the 2D world's events reach the 2D callbacks).
		struct HeldPhysicsEvent
		{
			PhysicsEvent Event{};
			bool Is2D = false;
		};

	}

	struct ScriptEngine::State
	{
		using InstanceKey = Detail::ScriptInstanceKey;
		// A fixed-step callback an instance holds until it has started: a physics callback of either world (M21), an
		// animation event (M23, Docs/Decisions/0034-m23-contract.md decision 31), or a joint's break (M37,
		// Docs/Decisions/0054-m37-joints.md decision 8).
		using HeldEvent = std::variant<Detail::HeldPhysicsEvent, AnimationEventRecord, PhysicsJointBreak>;
		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decision 2): OnLocaleChanged's arguments after self.
		struct LocaleChange
		{
			std::string_view Locale{};
			std::string_view Previous{};
		};

		struct Instance
		{
			AssetHandle Handle{};
			AssetRef<ScriptData> Script{};
			int Table = LUA_NOREF;
			std::string Name{}; // the entity's name, for diagnostics; the behaviour's Script.Define name is Script->Name
			int32_t Order = 0;
			// Unique per created instance, so a replacement under the same key is never mistaken for its predecessor.
			uint64_t Serial = 0;
			bool Active = false;
			// OnCreate was entered. No other callback (including OnDestroy) reaches a table before its OnCreate, even when
			// another instance's module top removes or toggles it while its batch is still being created.
			bool Created = false;
			// OnCreate was not entered because a memory or timeout fault of the creating slice was latched; it runs at the
			// next synchronization after recovery instead of being aborted half way (§11.1, M13 review F08).
			bool CreateDeferred = false;
			bool Started = false;
			bool FaultDisabled = false;
			bool DestroyPrepared = false;
			// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 2): the behaviour's class declares
			// RunsWhileWorldPaused = true, so its OnFixedUpdate, OnUpdate, OnLateUpdate and tasks run while the world is paused
			// and its tasks count the session's ticks. Read from the registration when the instance is created and when its
			// script is hot-reloaded.
			bool RunsWhileWorldPaused = false;
			std::map<std::string, VariantValue> Overrides{};
			// Physics callbacks (of both worlds, interleaved as they arrived) and animation events that arrived before OnStart,
			// in dispatch order; delivered after the start flush (§9.4 pairs).
			std::vector<HeldEvent> PendingEvents{};
		};

		// Evaluation and replay-predicate failures are reported without disabling the addressed instance. CallerOnly is an
		// external evaluation's (ScriptEngine::Evaluate; ADR 0038, friction F01): the caller's error, logged and kept in
		// CallerFailure for the caller, but neither retained in the error stream nor passed to the hosts or a test case;
		// one that stopped the VM (fatal) is the game's, and is published as ReportOnly publishes it.
		enum class FaultPolicy : uint8_t
		{
			DisableInstance,
			ReportOnly,
			CallerOnly
		};

		enum class ReferenceKind : uint8_t
		{
			Function,
			Task,
			Case
		};

		struct Reference
		{
			ReferenceKind Kind = ReferenceKind::Function;
			int Root = LUA_NOREF;
			lua_State* Thread = nullptr; // VM-owned, rooted by Root; never outlives Vm
			ScriptTaskOwner Owner{};
			ScriptExecutionOrigin Origin = ScriptExecutionOrigin::Gameplay;
			bool Running = false;
			bool Cancelled = false;
			bool Started = false;
			// M35 (decision 2): the thread counts the world's ticks (an instance's that does not run while the world is paused),
			// which hold while the world is paused, so it neither resumes nor comes due then; otherwise the session's ticks (a
			// test case's, a task of an instance that runs while paused, a task without an instance owner). Due is in its clock.
			bool WorldClock = false;
			uint64_t Due = 0;
			std::optional<Detail::ScriptTestWait> Wait{};
			std::optional<ScriptCaseResume> Terminal{};
		};

		// An unclaimed error that Test.ExpectScriptError can still claim for its case.
		struct ObservedError
		{
			ScriptReference Case{};
			ScriptError Error{};
		};

		struct ReloadPlan
		{
			std::vector<AssetHandle> Ordered{}; // dependency order: required modules first, then dependents
			std::map<AssetHandle, Ref<const ScriptData>> Scripts{};
		};

		struct PendingFailure
		{
			ScriptError Error{};
			ScriptTaskOwner Owner{};
			bool CaseBody = false;
			FaultPolicy Policy = FaultPolicy::DisableInstance;
		};

		explicit State(ScriptEngine& engine);
		~State();
		void AssertOwner() const;
		// Instance keys in canonical entity order and slot order, keeping removed instances at their last canonical anchor
		// until teardown. Linear in the scene with no per-entity allocation; a no-op while the scene revision and instance
		// membership are unchanged.
		void RefreshLifecycleOrder();
		// Sorted IDs of every entity with an instance, for allocation-free membership tests during canonical walks.
		[[nodiscard]] std::vector<UUID> SortedInstanceIDs() const;
		// The keys of the entity's instances, in slot order.
		[[nodiscard]] std::vector<InstanceKey> InstancesOf(UUID entity) const;
		// The instance's table when a script can address it (it has its table and is not being torn down; a fault-disabled
		// instance still can), else LUA_NOREF.
		[[nodiscard]] int FindLiveTable(InstanceKey key) const;
		// (ExecutionOrder, canonical entity, slot) order.
		[[nodiscard]] std::vector<InstanceKey> OrderedInstances(bool startedOnly);
		// The keys of every slot of `entities` whose script is set and that has no instance, in canonical entity order and
		// slot order.
		[[nodiscard]] std::vector<InstanceKey> MissingInstances(std::span<const UUID> entities) const;
		// Makes the instances (`instances` in canonical entity order and slot order), then runs their OnCreate in
		// (ExecutionOrder, canonical entity, slot) order. Once a nested entry has latched a memory or timeout fault of the
		// current slice, the batch stops: remaining instances stay uncreated and created instances defer their OnCreate, so
		// no innocent instance is left half initialized by the inherited fault.
		[[nodiscard]] Status CreateInstances(std::span<const InstanceKey> instances);
		// True when the instance exists with its table; false when its failure was already published or it vanished.
		[[nodiscard]] Result<bool> CreateInstance(InstanceKey key);
		// A nested entry of the current slice inherits a latched memory or timeout fault, so every later entry would fail
		// with it. Never true at the outermost level, where each entry recovers before the next one starts.
		[[nodiscard]] bool IsSliceFaulted() const;
		// Runs the deferred OnCreate callbacks in (ExecutionOrder, canonical entity, slot) order; a no-op inside a faulted
		// slice.
		void InvokeDeferredCreates();
		// `event` is a physics callback's (its other entity and contact), `animation` OnAnimationEvent's (its AnimationEvent),
		// `locale` OnLocaleChanged's (the new locale and the previous one; M38), `jointBreak` OnJointBreak's (its force and
		// torque; M37).
		void Invoke(InstanceKey key, std::string_view callback, const PhysicsEvent* event = nullptr, bool destroying = false,
			const AnimationEventRecord* animation = nullptr, const LocaleChange* locale = nullptr, const PhysicsJointBreak* jointBreak = nullptr);
		// The 3D callbacks (OnCollisionEnter, ...), or with `is2D` the 2D ones (OnCollisionEnter2D, ...; M21).
		void InvokePhysics(InstanceKey key, const PhysicsEvent& event, bool is2D = false);
		void InvokeAnimation(InstanceKey key, const AnimationEventRecord& event);
		void InvokeJointBreak(InstanceKey key, const PhysicsJointBreak& jointBreak);
		// OnPhysicsEvent's and OnPhysics2DEvent's delivery: every instance of the receiving entity, in slot order.
		void DeliverPhysics(const PhysicsEvent& event, bool is2D);
		// OnPhysicsDiagnostic's and OnPhysics2DDiagnostic's script error, its callback `callback`.
		void PublishPhysicsDiagnostic(const PhysicsDiagnostic& diagnostic, std::string_view callback);
		// M22 (C22.2, Docs/Decisions/0033-m22-contract.md decision 27): the event's callback (GetUiEventCallbackName) on the
		// instance when it is started and enabled and its behaviour defines it, with the event's target and value. True when
		// the callback returned true (the boolean), which stops the event's bubbling after the instance's entity.
		[[nodiscard]] bool InvokeUi(InstanceKey key, const UiEvent& event);
		// Delivers the physics callbacks (of both worlds) and animation events held for started instances: an entity's
		// instances together, in slot order, at the turn of its first in (ExecutionOrder, canonical entity, slot) order
		// (decision 17's entity callbacks), and dispatch order per instance; an instance removed or faulted by a callback
		// discards the rest of its held events.
		void DeliverPendingEvents();
		// Unreferences the instance's table, erases it and marks the lifecycle order for a refresh. Returns the next one.
		std::map<InstanceKey, Instance>::iterator EraseInstance(std::map<InstanceKey, Instance>::iterator instance);
		// OnFixedUpdate, OnUpdate or OnLateUpdate of the started instances, in (ExecutionOrder, canonical entity, slot) order;
		// while the host's frame state holds the world pause (M35, decision 2), only of those that run while it is paused.
		void Phase(std::string_view callback);
		// M35 (decision 2): whether a thread or task of `owner` counts the world's ticks (see Reference::WorldClock): an owner
		// with an entity whose instance (entity, slot) does not run while the world is paused, or no longer exists.
		[[nodiscard]] bool UsesWorldClock(const ScriptTaskOwner& owner) const;
		// The current tick of a clock: the host's session tick, or with `worldClock` the world's (Tick - WorldHeldTicks).
		[[nodiscard]] uint64_t GetClockTick(bool worldClock) const;
		// An error without an entity takes the owner's; an error of the owner's entity names the owner's slot
		// (ScriptError::Slot). The instance the error names is the one the DisableInstance policy disables.
		void Publish(ScriptError error, ScriptTaskOwner owner = {}, bool caseBody = false, bool alreadyLogged = false,
			FaultPolicy policy = FaultPolicy::DisableInstance);
		// The Script channel's Error entry of a published (or CallerOnly) error: its kind and message, then its callback,
		// entity and traceback, with the error's tick, entity and location as the log context. A CallerOnly error's entry is
		// marked answeredToCaller (LogContext::AnsweredToCaller), so "_meta"'s diagnostics leave it out (ADR 0038, F13).
		static void LogScriptError(const ScriptError& occurrence, bool answeredToCaller);
		void Publish(const Error& error, InstanceKey instance = {}, std::string_view callback = {});
		// Not in Dist. The changed script plus the loaded modules and attached behaviours that transitively require it,
		// in dependency order. Loads published assets only; executes no script code and publishes nothing.
		[[nodiscard]] Result<ReloadPlan> PlanReload(AssetHandle changed, std::span<const std::string> loadedModules);
		void Recover();
		[[nodiscard]] Result<ScriptCallResult> RunNative(const std::function<int(ScriptCall&)>& operation,
			ScriptExecutionOrigin origin, ScriptTaskOwner owner = {}, std::string_view callback = {});
		[[nodiscard]] static int InvokeNative(ScriptCall& call);
		[[nodiscard]] ScriptReference Retain(lua_State* state, int index, ReferenceKind kind);
		[[nodiscard]] Result<Reference*> FindReference(ScriptReference reference, ReferenceKind kind);
		[[nodiscard]] Reference* FindThread(lua_State* state);
		void DiscardThreadContents(Reference& reference);
		void Release(ScriptReference reference);
		[[nodiscard]] Result<ScriptCaseResume> Resume(ScriptReference thread);
		[[nodiscard]] Result<int> PrepareWait(ScriptCall& call, const Detail::ScriptTestWait& wait);
		[[nodiscard]] bool MatchError(ScriptCall& call, std::string_view pattern, std::string_view message);
		[[nodiscard]] bool ClaimError(ScriptCall& call, ScriptReference caseThread, std::string_view pattern);

		ScriptEngine& Engine; // enclosing owner, outlives State
		std::thread::id OwnerThread = std::this_thread::get_id();
		Scope<Sandbox> Vm{};
		Scope<TaskScheduler> Scheduler{};
		std::map<InstanceKey, Instance> Instances{};
		// Instance keys in last canonical entity order and slot order, retaining removed keys until their pending script
		// teardown has completed. A new instance is appended until the next refresh places it, so one destroyed before
		// then still tears down.
		std::vector<InstanceKey> LifecycleOrder{};
		uint64_t LifecycleRevision = 0;
		bool LifecycleDirty = true; // instance membership changed since the last refresh
		// Instances whose entity was observed inactive and that were not enabled since. Kept per slot, so a slot torn down
		// while its entity is inactive is recreated only when the entity is enabled again, whatever its other slots did.
		std::set<InstanceKey> DisabledHistory{};
		// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 8): how many SynchronizeInstances calls are running,
		// the outermost one counting 1. Only the outermost one re-enables an instance whose entity was switched on again
		// before its disable was flushed: a nested one (a callback of the outer one's transitions that switches entities)
		// leaves that to the next outermost synchronization, so no instance receives OnEnable before the OnDisable the outer
		// one observed, and a switch back on inside a destroy flush's own synchronization still completes that flush's
		// teardown.
		uint32_t SynchronizationDepth = 0;
		std::map<uint64_t, Reference> References{};
		// The index in References of each live coroutine (Reference::Thread, unique while rooted), so FindThread, which
		// CheckExecution runs around every binding call outside a task or case body, is logarithmic in the references
		// retained (a suite's cases alone retain thousands). A lookup structure only, never iterated: CreateThread adds
		// an entry and DiscardThreadContents removes it.
		std::map<const lua_State*, uint64_t> ThreadReferences{};
		uint64_t NextReference = 1;
		uint64_t NextInstanceSerial = 1;
		uint32_t CreationDepth = 0;
		bool HasDeferredCreates = false; // some instance has CreateDeferred set
		bool Initialized = false;
		bool Stopped = false;
		bool Stopping = false;
		// M38 (decision 2): an OnLocaleChanged dispatch runs (ScriptEngine::NotifyLocaleChanged), so Loc.SetLocale refuses.
		bool LocaleDispatching = false;
		// Stack-scoped, exception-neutral back-references restored before a protected host call returns.
		const std::function<int(ScriptCall&)>* NativeOperation = nullptr;
		ScriptTaskOwner CurrentOwner{};
		lua_State* ActiveThread = nullptr; // stack-scoped executing state; never retained after the outer entry
		ScriptReference CurrentThread{};   // logical task/case during its body or a protected wait-predicate poll
		IScriptTestHost* TestHost = nullptr;
		bool SuiteActive = false;
		bool Collecting = false;
		uint32_t SuiteTimeout = 0;
		ScriptCollectedSuite Suite{};
		// The names of Suite.Cases, so Test.Case checks a name's uniqueness in logarithmic time. A lookup structure
		// only, never iterated: Suite.Cases keeps the registration order. Cleared wherever Suite is replaced.
		std::set<std::string, std::less<>> SuiteCaseNames{};
		// The running case receives error attribution and test signals from any code, but owns only the work executing on
		// its thread or on tasks it spawned; gameplay callbacks and their tasks are never cancelled with the case.
		ScriptReference ActiveCase{};
		// Set by Test.Fail/Test.Skip; a lifecycle callback that raised one ends its case instead of faulting its instance.
		bool TestSignalRaised = false;
		// At most MaxScriptTestUnclaimedErrors (ScriptLimits.h); a claim removes its entry.
		std::vector<ObservedError> ObservedErrors{};
		std::optional<PendingFailure> PendingSafetyFailure{};
		// The last CallerOnly failure Publish logged without publishing it, bounded (BoundScriptError) and with its owner's
		// entity, slot and tick; ScriptEngine::Evaluate resets it before it runs and hands it to its caller.
		std::optional<ScriptError> CallerFailure{};
	};

}
