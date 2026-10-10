#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
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
		const auto canonical = Engine.GetHost().GetScene().GetCanonicalOrder();
		std::map<UUID, size_t> positions;
		for (size_t index = 0; index < canonical.size(); ++index)
			positions.emplace(canonical[index], index);
		// Scene destruction removes lookup/hierarchy entries immediately. Keep those tombstones at their last
		// canonical anchor, while refreshing all surviving entities and any newly instantiated subtree.
		std::vector<std::vector<UUID>> pending(canonical.size() + 1);
		size_t anchor = canonical.size();
		for (auto current = LifecycleOrder.rbegin(); current != LifecycleOrder.rend(); ++current)
		{
			const auto position = positions.find(*current);
			if (position != positions.end())
				anchor = position->second;
			else if (Instances.contains(*current))
				pending[anchor].push_back(*current);
		}
		LifecycleOrder.clear();
		for (size_t index = 0; index <= canonical.size(); ++index)
		{
			LifecycleOrder.insert(LifecycleOrder.end(), pending[index].rbegin(), pending[index].rend());
			if (index < canonical.size())
				LifecycleOrder.push_back(canonical[index]);
		}
	}

	std::vector<UUID> ScriptEngine::State::OrderedInstances(bool startedOnly) const
	{
		std::vector<UUID> result;
		Scene& scene = Engine.GetHost().GetScene();
		for (const UUID id : scene.GetCanonicalOrder())
		{
			const auto instance = Instances.find(id);
			if (instance == Instances.end() || instance->second.FaultDisabled || instance->second.DestroyPrepared || !instance->second.Active
				|| (startedOnly && !instance->second.Started))
				continue;
			const Entity entity = scene.FindEntityByID(id);
			if (entity && entity.IsActive())
				result.push_back(id);
		}
		std::stable_sort(result.begin(), result.end(), [this](UUID left, UUID right)
		{
			return Instances.find(left)->second.Order < Instances.find(right)->second.Order;
		});
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

	void ScriptEngine::State::Publish(ScriptError error, ScriptTaskOwner owner, bool caseBody, bool alreadyLogged)
	{
		const bool nested = Vm != nullptr && Detail::SandboxAccess::GetWatchdog(*Vm)->GetDepth() != 0;
		if (nested && (error.Kind == ScriptErrorKind::Timeout || error.Kind == ScriptErrorKind::Memory))
		{
			// Safety faults remain latched across nested protected entries. The outer owner publishes once after unwind.
			if (!PendingSafetyFailure.has_value())
				PendingSafetyFailure = PendingFailure{ std::move(error), owner, caseBody };
			return;
		}
		if (!nested && PendingSafetyFailure.has_value())
		{
			error = std::move(PendingSafetyFailure->Error);
			owner = PendingSafetyFailure->Owner;
			caseBody = PendingSafetyFailure->CaseBody;
			PendingSafetyFailure.reset();
			alreadyLogged = false;
		}
		if (!error.Entity.IsValid())
			error.Entity = owner.Entity;
		error.Tick = Engine.GetHost().GetFrameState().Tick;
		const auto instance = Instances.find(error.Entity);
		if (instance != Instances.end())
		{
			if (error.Script.empty() && instance->second.Script != nullptr)
				error.Script = instance->second.Script->SourceMap.Path;
			if (error.EntityName.empty())
				error.EntityName = instance->second.Name;
			instance->second.FaultDisabled = true;
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
		const ScriptReference currentCase = !owner.Case.IsNull() ? owner.Case : ActiveCase;
		if (!currentCase.IsNull())
			ObservedErrors.push_back({ currentCase, occurrence, !fatal && !caseBody, false });
		Engine.GetHost().OnScriptError(occurrence, fatal);
		if (TestHost != nullptr)
			TestHost->OnScriptError(occurrence, fatal);
		if (Engine.m_Specification.Settings.PauseOnError && Engine.GetHost().GetEnvironment().IsEditor)
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
