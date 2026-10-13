#pragma once

#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/TaskScheduler.h"

#include <lua.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace Engine {

	struct ScriptEngine::State
	{
		struct Instance
		{
			AssetHandle Handle{};
			AssetRef<ScriptData> Script{};
			int Table = LUA_NOREF;
			std::string Name{};
			int32_t Order = 0;
			// Unique per created instance, so a replacement under the same entity ID is never mistaken for its predecessor.
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
			std::map<std::string, VariantValue> Overrides{};
			// Physics callbacks that arrived before OnStart, in dispatch order; delivered after the start flush (§9.4 pairs).
			std::vector<PhysicsEvent> PendingPhysics{};
		};

		// Evaluation and replay-predicate failures are reported without disabling the addressed entity's behaviour.
		enum class FaultPolicy : uint8_t
		{
			DisableInstance,
			ReportOnly
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
		// Instance IDs in canonical order, keeping removed instances at their last canonical anchor until teardown. Linear
		// in the scene with no per-entity allocation; a no-op while the scene revision and instance membership are unchanged.
		void RefreshLifecycleOrder();
		// Sorted IDs of every live instance, for allocation-free membership tests during canonical walks.
		[[nodiscard]] std::vector<UUID> SortedInstanceIDs() const;
		[[nodiscard]] std::vector<UUID> OrderedInstances(bool startedOnly);
		// Makes the instances, then runs their OnCreate in (ExecutionOrder, canonical) order. Once a nested entry has latched
		// a memory or timeout fault of the current slice, the batch stops: remaining entities stay uncreated and created
		// instances defer their OnCreate, so no innocent instance is left half initialized by the inherited fault.
		[[nodiscard]] Status CreateInstances(std::span<const UUID> entities);
		// True when the instance exists with its table; false when its failure was already published or it vanished.
		[[nodiscard]] Result<bool> CreateInstance(UUID entity);
		// A nested entry of the current slice inherits a latched memory or timeout fault, so every later entry would fail
		// with it. Never true at the outermost level, where each entry recovers before the next one starts.
		[[nodiscard]] bool IsSliceFaulted() const;
		// Runs the deferred OnCreate callbacks in (ExecutionOrder, canonical) order; a no-op inside a faulted slice.
		void InvokeDeferredCreates();
		void Invoke(UUID entity, std::string_view callback, const PhysicsEvent* event = nullptr, bool destroying = false);
		void InvokePhysics(const PhysicsEvent& event);
		// Delivers physics callbacks held for started instances, in (ExecutionOrder, canonical) order and dispatch order
		// per instance; an instance removed or faulted by a callback discards the rest of its held events.
		void DeliverPendingPhysics();
		void Phase(std::string_view callback);
		void Publish(ScriptError error, ScriptTaskOwner owner = {}, bool caseBody = false, bool alreadyLogged = false,
			FaultPolicy policy = FaultPolicy::DisableInstance);
		void Publish(const Error& error, UUID entity = {}, std::string_view callback = {});
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
		std::map<UUID, Instance> Instances{};
		// Instance IDs in last canonical order, retaining removed IDs until their pending script teardown has completed.
		// A new instance is appended until the next refresh places it, so one destroyed before then still tears down.
		std::vector<UUID> LifecycleOrder{};
		uint64_t LifecycleRevision = 0;
		bool LifecycleDirty = true; // instance membership changed since the last refresh
		std::set<UUID> DisabledHistory{};
		std::map<uint64_t, Reference> References{};
		uint64_t NextReference = 1;
		uint64_t NextInstanceSerial = 1;
		uint32_t CreationDepth = 0;
		bool HasDeferredCreates = false; // some instance has CreateDeferred set
		bool Initialized = false;
		bool Stopped = false;
		bool Stopping = false;
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
		// The running case receives error attribution and test signals from any code, but owns only the work executing on
		// its thread or on tasks it spawned; gameplay callbacks and their tasks are never cancelled with the case.
		ScriptReference ActiveCase{};
		// Set by Test.Fail/Test.Skip; a lifecycle callback that raised one ends its case instead of faulting its instance.
		bool TestSignalRaised = false;
		// At most MaxScriptTestUnclaimedErrors (ScriptLimits.h); a claim removes its entry.
		std::vector<ObservedError> ObservedErrors{};
		std::optional<PendingFailure> PendingSafetyFailure{};
	};

}
