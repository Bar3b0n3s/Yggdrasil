#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Scripting/Watchdog.h"

#include <algorithm>
#include <format>
#include <limits>
#include <utility>

namespace Engine {

	ScriptEngine::State::State(ScriptEngine& engine)
		: Engine(engine)
	{
	}

	ScriptEngine::State::~State()
	{
		AssertOwner();
		Scheduler.reset();
		if (Vm == nullptr)
			return;
		lua_State* state = Detail::SandboxAccess::MainCall(*Vm).State;
		for (const auto& [id, instance] : Instances)
		{
			static_cast<void>(id);
			if (instance.Table != LUA_NOREF)
				lua_unref(state, instance.Table);
		}
		for (const auto& [index, reference] : References)
		{
			static_cast<void>(index);
			if (reference.Root != LUA_NOREF)
				lua_unref(state, reference.Root);
		}
	}

	void ScriptEngine::State::AssertOwner() const
	{
		ENGINE_CORE_ASSERT(OwnerThread == std::this_thread::get_id(), "ScriptEngine must stay on its owner thread");
	}

	void ScriptEngine::State::RefreshLifecycleOrder()
	{
		const Scene& scene = Engine.GetHost().GetScene();
		const uint64_t revision = scene.GetRevision();
		if (Instances.empty())
		{
			LifecycleOrder.clear();
			LifecycleRevision = revision;
			LifecycleDirty = false;
			return;
		}
		// Every structural scene change increments the revision, and membership changes mark the order dirty.
		if (!LifecycleDirty && revision == LifecycleRevision)
			return;
		const std::vector<UUID> members = SortedInstanceIDs();
		std::vector<UUID> alive;
		for (const UUID id : scene.GetCanonicalOrder())
		{
			if (std::binary_search(members.begin(), members.end(), id))
				alive.push_back(id);
		}
		std::vector<UUID> aliveSorted = alive;
		std::sort(aliveSorted.begin(), aliveSorted.end());
		// Scene destruction removes lookup/hierarchy entries immediately. Keep each removed instance at its last anchor
		// (the next surviving instance after it in the previous order, or the end), preserving their relative order.
		std::map<UUID, std::vector<UUID>> anchored;
		std::set<UUID> placed;
		UUID anchor{};
		for (auto current = LifecycleOrder.rbegin(); current != LifecycleOrder.rend(); ++current)
		{
			if (std::binary_search(aliveSorted.begin(), aliveSorted.end(), *current))
				anchor = *current;
			else if (std::binary_search(members.begin(), members.end(), *current) && placed.insert(*current).second)
				anchored[anchor].push_back(*current);
		}
		std::vector<std::pair<uint64_t, UUID>> unanchored;
		for (const auto& [id, instance] : Instances)
		{
			if (!std::binary_search(aliveSorted.begin(), aliveSorted.end(), id) && !placed.contains(id))
				unanchored.emplace_back(instance.Serial, id);
		}
		std::sort(unanchored.begin(), unanchored.end());
		LifecycleOrder.clear();
		const auto emitAnchored = [this, &anchored](UUID key)
		{
			const auto found = anchored.find(key);
			if (found != anchored.end())
				LifecycleOrder.insert(LifecycleOrder.end(), found->second.rbegin(), found->second.rend());
		};
		for (const UUID id : alive)
		{
			emitAnchored(id);
			LifecycleOrder.push_back(id);
		}
		emitAnchored(UUID{});
		for (const auto& [serial, id] : unanchored)
		{
			static_cast<void>(serial);
			LifecycleOrder.push_back(id);
		}
		LifecycleRevision = revision;
		LifecycleDirty = false;
	}

	std::vector<UUID> ScriptEngine::State::SortedInstanceIDs() const
	{
		std::vector<UUID> result;
		result.reserve(Instances.size());
		for (const auto& [id, instance] : Instances)
		{
			static_cast<void>(instance);
			result.push_back(id); // std::map iterates in key order
		}
		return result;
	}

	std::vector<UUID> ScriptEngine::State::OrderedInstances(bool startedOnly)
	{
		RefreshLifecycleOrder();
		Scene& scene = Engine.GetHost().GetScene();
		std::vector<std::pair<int32_t, UUID>> eligible;
		for (const UUID id : LifecycleOrder)
		{
			const auto instance = Instances.find(id);
			if (instance == Instances.end() || instance->second.FaultDisabled || instance->second.DestroyPrepared || !instance->second.Active
				|| (startedOnly && !instance->second.Started))
				continue;
			const Entity entity = scene.FindEntityByID(id);
			if (entity && entity.IsActive())
				eligible.emplace_back(instance->second.Order, id);
		}
		// LifecycleOrder lists surviving instances in canonical order, so a stable sort gives (ExecutionOrder, canonical).
		std::stable_sort(eligible.begin(), eligible.end(), [](const auto& left, const auto& right)
		{
			return left.first < right.first;
		});
		std::vector<UUID> result;
		result.reserve(eligible.size());
		for (const auto& [order, id] : eligible)
		{
			static_cast<void>(order);
			result.push_back(id);
		}
		return result;
	}

	int ScriptEngine::State::InvokeNative(ScriptCall& call)
	{
		ENGINE_CORE_VERIFY(call.Engine != nullptr && call.Engine->m_State->NativeOperation != nullptr,
			"Missing protected engine operation");
		return (*call.Engine->m_State->NativeOperation)(call);
	}

	Result<ScriptCallResult> ScriptEngine::State::RunNative(const std::function<int(ScriptCall&)>& operation,
		ScriptExecutionOrigin origin, ScriptTaskOwner owner, std::string_view callback)
	{
		AssertOwner();
		if (Vm == nullptr || Stopped)
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*Vm, origin));
		struct ExecutionScope
		{
			State& StateOwner;
			const std::function<int(ScriptCall&)>* PreviousOperation = nullptr;
			ScriptTaskOwner PreviousOwner{};
			lua_State* PreviousThread = nullptr;
			bool HasContext = false;
			~ExecutionScope()
			{
				StateOwner.NativeOperation = PreviousOperation;
				StateOwner.CurrentOwner = PreviousOwner;
				StateOwner.ActiveThread = PreviousThread;
				if (HasContext)
					Detail::SandboxAccess::PopContext(*StateOwner.Vm);
				Detail::SandboxAccess::LeaveProtected(*StateOwner.Vm);
			}
		} scope{ *this, NativeOperation, CurrentOwner, ActiveThread, false };
		NativeOperation = &operation;
		CurrentOwner = owner;
		Detail::SandboxExecutionContext context{};
		context.Callback = std::string(callback);
		context.Entity = owner.Entity;
		context.Tick = Engine.GetHost().GetFrameState().Tick;
		const auto instance = Instances.find(owner.Entity);
		if (instance != Instances.end())
		{
			context.Script = instance->second.Script != nullptr ? instance->second.Script->SourceMap.Path : std::string();
			context.EntityName = instance->second.Name;
		}
		ENGINE_TRY(Detail::SandboxAccess::PushContext(*Vm, context));
		scope.HasContext = true;
		ENGINE_TRY_ASSIGN(auto call, Detail::SandboxAccess::ThreadCall(*Vm, ActiveThread != nullptr ? ActiveThread : Detail::SandboxAccess::MainCall(*Vm).State));
		ActiveThread = call.State;
		call.MemberName = callback;
		return Lua::ProtectedCall(call, &State::InvokeNative);
	}

	void ScriptEngine::State::Publish(ScriptError error, ScriptTaskOwner owner, bool caseBody, bool alreadyLogged, FaultPolicy policy)
	{
		const bool nested = Vm != nullptr && Detail::SandboxAccess::GetWatchdog(*Vm)->GetDepth() != 0;
		if (nested && (error.Kind == ScriptErrorKind::Timeout || error.Kind == ScriptErrorKind::Memory))
		{
			// Safety faults remain latched across nested protected entries. The outer owner publishes once after unwind.
			if (!PendingSafetyFailure.has_value())
				PendingSafetyFailure = PendingFailure{ std::move(error), owner, caseBody, policy };
			return;
		}
		if (!nested && PendingSafetyFailure.has_value())
		{
			error = std::move(PendingSafetyFailure->Error);
			owner = PendingSafetyFailure->Owner;
			caseBody = PendingSafetyFailure->CaseBody;
			policy = PendingSafetyFailure->Policy;
			PendingSafetyFailure.reset();
			alreadyLogged = false;
		}
		if (!error.Entity.IsValid())
			error.Entity = owner.Entity;
		error.Tick = Engine.GetHost().GetFrameState().Tick;
		const auto instance = Instances.find(error.Entity);
		if (instance != Instances.end())
		{
			if (error.EntityName.empty())
				error.EntityName = instance->second.Name;
		}
		// An external evaluation is not a callback of the addressed instance (§11.7 disables only the faulting instance).
		// Its failure is recorded and logged, but never attributed to the behaviour's file, disables it or cancels its tasks.
		if (instance != Instances.end() && policy == FaultPolicy::DisableInstance)
		{
			if (error.Script.empty() && instance->second.Script != nullptr)
				error.Script = instance->second.Script->SourceMap.Path;
			instance->second.FaultDisabled = true;
			instance->second.PendingPhysics.clear();
			if (Scheduler != nullptr)
				Scheduler->CancelEntity(error.Entity);
			if (error.Kind == ScriptErrorKind::Memory && instance->second.Table != LUA_NOREF && !nested)
			{
				lua_unref(Detail::SandboxAccess::MainCall(*Vm).State, instance->second.Table);
				instance->second.Table = LUA_NOREF;
			}
		}
		Recover();
		const bool fatal = Vm != nullptr && Vm->IsStopped();
		const ScriptError occurrence = Engine.m_Errors.Add(std::move(error));
		if (!alreadyLogged)
		{
			// Physics logs before notifying its listener. Other errors log here; host histories are data-only copies.
			LogContext context = LogContextScope::GetCurrent();
			context.Tick = occurrence.Tick;
			context.Entity = occurrence.Entity;
			context.ScriptFile = occurrence.Script;
			context.ScriptLine = occurrence.Line;
			const LogContextScope scope(context);
			std::string message = std::format("{}: {}", ScriptErrorKindToString(occurrence.Kind), occurrence.Message);
			if (!occurrence.Callback.empty())
				message += std::format("\nCallback: {}", occurrence.Callback);
			if (!occurrence.EntityName.empty())
				message += std::format("\nEntity: {} ({})", occurrence.EntityName, occurrence.Entity);
			for (const ScriptTraceFrame& frame : occurrence.Traceback)
				message += std::format("\n  at {}:{} ({})", frame.Script, frame.Line, frame.Function);
			const int line = static_cast<int>(std::min(occurrence.Line, static_cast<uint32_t>(std::numeric_limits<int>::max())));
			Detail::LogWrite(LogChannel::Script, LogLevel::Error, occurrence.Script.c_str(), line, occurrence.Callback.c_str(), "{}", message);
		}
		// Only a nonfatal error outside the case body can be claimed. Past the cap it cannot, and the runner reports it as
		// an unexpected error of the case.
		const ScriptReference currentCase = !owner.Case.IsNull() ? owner.Case : ActiveCase;
		if (!currentCase.IsNull() && !fatal && !caseBody && ObservedErrors.size() < MaxScriptTestUnclaimedErrors)
			ObservedErrors.push_back({ currentCase, occurrence });
		Engine.GetHost().OnScriptError(occurrence, fatal);
		if (TestHost != nullptr)
			TestHost->OnScriptError(occurrence, fatal);
		// Test runs never pause: the runner owns time, so Editor, Release and Dist runs observe the same audio and alpha.
		// External evaluations report their failure to the caller and leave the running game's time untouched.
		if (Engine.m_Specification.Settings.PauseOnError && Engine.GetHost().GetEnvironment().IsEditor && !Engine.IsTestMode()
			&& policy == FaultPolicy::DisableInstance)
			Engine.GetHost().RequestPause();
		if (fatal)
		{
			Stopped = true;
			if (Scheduler != nullptr)
				Scheduler->CancelAll();
		}
	}

	void ScriptEngine::State::Publish(const Error& error, UUID entity, std::string_view callback)
	{
		ScriptError report{};
		report.Kind = error.GetCode() == ErrorCode::CompileFailed ? ScriptErrorKind::Compile
			: error.GetCode() == ErrorCode::Timeout               ? ScriptErrorKind::Timeout
																  : ScriptErrorKind::Runtime;
		report.Message = error.ToString();
		report.Entity = entity;
		report.Callback = std::string(callback);
		const ErrorLocation& location = error.GetLocation();
		report.Script = location.File;
		report.Line = location.Line;
		report.Column = location.Column;
		report.JsonPointer = location.JsonPointer.value_or("");
		Publish(std::move(report), { .Entity = entity, .Case = CurrentOwner.Case });
	}

	void ScriptEngine::State::Recover()
	{
		if (Vm == nullptr || Stopped || !Vm->GetMemoryState().NeedsRecovery)
			return;
		// A nested failure cannot collect/reset the VM while an outer native/Luau frame is still live.
		if (Detail::SandboxAccess::GetWatchdog(*Vm)->GetDepth() != 0)
			return;
		const Status recovered = Detail::SandboxAccess::RecoverMemory(*Vm);
		if (!recovered)
		{
			Stopped = true;
			if (Scheduler != nullptr)
				Scheduler->CancelAll();
		}
	}

	ScriptReference ScriptEngine::State::Retain(lua_State* state, int index, ReferenceKind kind)
	{
		ENGINE_CORE_VERIFY(NextReference != std::numeric_limits<uint64_t>::max(), "Script reference space exhausted");
		const ScriptReference id{ Engine.GetGeneration(), NextReference++ };
		Reference reference{};
		reference.Kind = kind;
		reference.Root = lua_ref(state, index);
		References.emplace(id.Index, reference);
		return id;
	}

	Result<ScriptEngine::State::Reference*> ScriptEngine::State::FindReference(ScriptReference reference, ReferenceKind kind)
	{
		if (reference.IsNull() || reference.VMGeneration != Engine.GetGeneration())
			return MakeError(ErrorCode::InvalidArgument, "The reference belongs to a different script VM");
		const auto found = References.find(reference.Index);
		if (found == References.end() || found->second.Kind != kind || found->second.Cancelled)
			return MakeError(ErrorCode::InvalidArgument, "The script reference is released or has the wrong kind");
		return &found->second;
	}

	ScriptEngine::State::Reference* ScriptEngine::State::FindThread(lua_State* state)
	{
		for (auto& [index, reference] : References)
		{
			static_cast<void>(index);
			if (reference.Thread == state)
				return &reference;
		}
		return nullptr;
	}

	void ScriptEngine::State::DiscardThreadContents(Reference& reference)
	{
		ENGINE_CORE_ASSERT(!reference.Running, "An executing coroutine must unwind before releasing its roots");
		const auto wait = reference.Wait;
		reference.Wait.reset();
		if (wait.has_value())
		{
			if (wait->Kind == Detail::ScriptWaitKind::Audio && TestHost != nullptr)
				TestHost->CancelAudioCapture();
			Release(wait->Predicate);
		}
		if (reference.Root != LUA_NOREF)
			lua_unref(Detail::SandboxAccess::MainCall(*Vm).State, reference.Root);
		reference.Root = LUA_NOREF;
		reference.Thread = nullptr;
	}

	void ScriptEngine::State::Release(ScriptReference reference)
	{
		if (Vm == nullptr || reference.VMGeneration != Engine.GetGeneration())
			return;
		const auto found = References.find(reference.Index);
		if (found == References.end())
			return;
		if (found->second.Running)
		{
			found->second.Cancelled = true;
			return;
		}
		DiscardThreadContents(found->second);
		References.erase(found);
	}

}
