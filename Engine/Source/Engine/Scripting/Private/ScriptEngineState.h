#pragma once

#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/TaskScheduler.h"

#include <lua.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
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
			bool Active = false;
			bool Started = false;
			bool FaultDisabled = false;
			bool DestroyPrepared = false;
			std::map<std::string, VariantValue> Overrides{};
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

		struct ObservedError
		{
			ScriptReference Case{};
			ScriptError Error{};
			bool Claimable = false;
			bool Claimed = false;
		};

		struct PendingFailure
		{
			ScriptError Error{};
			ScriptTaskOwner Owner{};
			bool CaseBody = false;
		};

		explicit State(ScriptEngine& engine);
		~State();
		void AssertOwner() const;
		void RefreshLifecycleOrder();
		[[nodiscard]] std::vector<UUID> OrderedInstances(bool startedOnly) const;
		[[nodiscard]] Status CreateInstances(std::span<const UUID> entities);
		[[nodiscard]] Status CreateInstance(UUID entity);
		void Invoke(UUID entity, std::string_view callback, const PhysicsEvent* event = nullptr, bool destroying = false);
		void Phase(std::string_view callback);
		void Publish(ScriptError error, ScriptTaskOwner owner = {}, bool caseBody = false, bool alreadyLogged = false);
		void Publish(const Error& error, UUID entity = {}, std::string_view callback = {});
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
		// Last canonical order, retaining removed IDs until their pending script teardown has completed.
		std::vector<UUID> LifecycleOrder{};
		std::set<UUID> DisabledHistory{};
		std::map<uint64_t, Reference> References{};
		uint64_t NextReference = 1;
		uint32_t CreationDepth = 0;
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
		ScriptReference ActiveCase{};
		std::vector<ObservedError> ObservedErrors{};
		std::optional<PendingFailure> PendingSafetyFailure{};
	};

}
