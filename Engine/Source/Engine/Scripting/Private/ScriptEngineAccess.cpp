#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace Engine {

	namespace Detail {

		Sandbox& ScriptEngineAccess::GetSandbox(ScriptEngine& engine)
		{
			engine.m_State->AssertOwner();
			ENGINE_CORE_VERIFY(engine.m_State->Vm != nullptr, "ScriptEngine has no sandbox");
			return *engine.m_State->Vm;
		}

		Status ScriptEngineAccess::PushInstance(ScriptCall& call, UUID entity)
		{
			if (call.Engine == nullptr)
				return MakeError(ErrorCode::InvalidState, "An instance requires an active script engine");
			const auto found = call.Engine->m_State->Instances.find(entity);
			if (found == call.Engine->m_State->Instances.end() || found->second.Table == LUA_NOREF || found->second.DestroyPrepared)
				lua_pushnil(call.State);
			else
				lua_getref(call.State, found->second.Table);
			return {};
		}

		Result<ScriptReference> ScriptEngineAccess::RetainFunction(ScriptCall& call, int index)
		{
			if (call.Engine == nullptr || call.Engine->IsStopped())
				return MakeError(ErrorCode::InvalidState, "A function reference requires a running script engine");
			if (!lua_isfunction(call.State, index))
				return MakeError(ErrorCode::InvalidArgument, "Expected a function");
			return call.Engine->m_State->Retain(call.State, index, ScriptEngine::State::ReferenceKind::Function);
		}

		Status ScriptEngineAccess::PushFunction(ScriptCall& call, ScriptReference reference)
		{
			if (call.Engine == nullptr)
				return MakeError(ErrorCode::InvalidState, "A function reference requires an active script engine");
			ENGINE_TRY_ASSIGN(auto found, call.Engine->m_State->FindReference(reference, ScriptEngine::State::ReferenceKind::Function));
			lua_getref(call.State, found->Root);
			return {};
		}

		ScriptTaskOwner ScriptEngineAccess::GetOwner(const ScriptCall& call)
		{
			if (call.Engine == nullptr)
				return {};
			ScriptTaskOwner owner = call.Engine->m_State->CurrentOwner;
			// Engine entries set ownership explicitly, including an ownerless nested evaluation. Diagnostic contexts
			// may still describe the outer caller and must never transfer its task ownership into that evaluation.
			if (owner.Case.IsNull())
				owner.Case = call.Engine->m_State->ActiveCase;
			return owner;
		}

		lua_State* ScriptEngineAccess::ExchangeActiveThread(ScriptEngine& engine, lua_State* thread)
		{
			engine.m_State->AssertOwner();
			return std::exchange(engine.m_State->ActiveThread, thread);
		}

		Status ScriptEngineAccess::CheckExecution(const ScriptCall& call)
		{
			if (call.Engine == nullptr)
				return {};
			auto& state = *call.Engine->m_State;
			state.AssertOwner();
			const auto active = state.References.find(state.CurrentThread.Index);
			const auto* thread = active != state.References.end() ? &active->second : state.FindThread(call.State);
			if (thread != nullptr && thread->Cancelled)
				return MakeError(ErrorCode::Cancelled, "The running script task was cancelled");
			const auto owner = GetOwner(call);
			const auto caseThread = state.References.find(owner.Case.Index);
			if (!owner.Case.IsNull() && caseThread != state.References.end() && caseThread->second.Terminal)
				return MakeError(ErrorCode::Cancelled, "The test case has already ended: {}", caseThread->second.Terminal->Message);
			return {};
		}

		void ScriptEngineAccess::PublishTaskFailure(ScriptEngine& engine, const Error& error, ScriptTaskOwner owner)
		{
			engine.m_State->AssertOwner();
			ScriptError failure{};
			failure.Kind = error.GetCode() == ErrorCode::Timeout ? ScriptErrorKind::Timeout : ScriptErrorKind::Runtime;
			failure.Message = error.ToString();
			failure.Callback = "Task";
			failure.Entity = owner.Entity;
			const auto& location = error.GetLocation();
			failure.Script = location.File;
			failure.Line = location.Line;
			failure.Column = location.Column;
			failure.JsonPointer = location.JsonPointer.value_or("");
			engine.m_State->Publish(std::move(failure), owner);
		}

		Status ScriptEngineAccess::RegisterTestCase(ScriptCall& call, int functionIndex, std::string_view name,
			uint32_t timeoutTicks, const ErrorLocation& location)
		{
			if (call.Engine == nullptr || !call.Engine->IsTestMode() || !call.Engine->m_State->Collecting)
				return MakeError(ErrorCode::InvalidState, "Test.Case is only available while collecting a suite");
			auto& state = *call.Engine->m_State;
			if (name.empty() || std::any_of(state.Suite.Cases.begin(), state.Suite.Cases.end(), [name](const ScriptTestCase& test)
			{
				return test.Name == name;
			}))
				return MakeError(ErrorCode::InvalidArgument, "Test case names must be nonempty and unique within the suite");
			ENGINE_TRY_ASSIGN(auto function, RetainFunction(call, functionIndex));
			state.Suite.Cases.push_back({ std::string(name), function, timeoutTicks != 0 ? timeoutTicks : state.SuiteTimeout,
				location.File, location.Line });
			return {};
		}

		Result<int> ScriptEngineAccess::ArmTestWait(ScriptCall& call, const ScriptTestWait& wait)
		{
			if (call.Engine == nullptr || !call.Engine->IsTestMode() || call.Engine->GetTestHost() == nullptr)
				return MakeError(ErrorCode::InvalidState, "Test waits require an active test run");
			return call.Engine->m_State->PrepareWait(call, wait);
		}

		Status ScriptEngineAccess::EndTestCase(ScriptCall& call, const ScriptTestReport& report)
		{
			if (call.Engine == nullptr || call.Engine->GetTestHost() == nullptr)
				return MakeError(ErrorCode::InvalidState, "Test outcomes require an active case");
			auto& state = *call.Engine->m_State;
			const ScriptReference id = GetOwner(call).Case;
			ENGINE_TRY_ASSIGN(auto thread, state.FindReference(id, ScriptEngine::State::ReferenceKind::Case));
			if (!thread->Terminal.has_value())
			{
				ScriptCaseResume terminal{};
				terminal.State = report.Signal == ScriptTestSignal::Skip ? ScriptCaseState::Skipped : ScriptCaseState::Failed;
				terminal.Message = report.Message;
				terminal.File = report.File;
				terminal.Line = report.Line;
				thread->Terminal = std::move(terminal);
				state.TestHost->Report(report);
				state.Scheduler->CancelCase(id);
			}
			return {};
		}

		Status ScriptEngineAccess::ArmTaskWait(ScriptCall& call, uint64_t ticks)
		{
			if (call.Engine == nullptr)
				return MakeError(ErrorCode::InvalidState, "Task waits require an active script engine");
			auto* thread = call.Engine->m_State->FindThread(call.State);
			if (thread == nullptr || !thread->Running || !lua_isyieldable(call.State))
				return MakeError(ErrorCode::Script, "Task.Wait can only be used inside Task.Spawn, Task.Delay or a Test.Case body");
			const uint64_t now = call.Engine->GetHost().GetFrameState().Tick;
			const uint64_t delay = std::max(uint64_t{ 1 }, ticks);
			if (delay > std::numeric_limits<uint64_t>::max() - now)
				return MakeError(ErrorCode::InvalidArgument, "The wait exceeds the simulation tick range");
			thread->Due = now + delay;
			ScriptTestWait wait{};
			wait.Kind = ScriptWaitKind::Ticks;
			wait.Ticks = delay;
			thread->Wait = std::move(wait);
			return {};
		}

		Result<ScriptReference> ScriptEngineAccess::CreateThread(ScriptEngine& engine, ScriptReference function,
			ScriptTaskOwner owner, ScriptThreadKind kind)
		{
			auto& state = *engine.m_State;
			state.AssertOwner();
			if (engine.IsStopped() || engine.IsReadOnly())
				return MakeError(ErrorCode::InvalidState, "Tasks cannot run in a stopped or read-only engine");
			ENGINE_TRY(state.FindReference(function, ScriptEngine::State::ReferenceKind::Function));
			if (owner.Entity.IsValid() && !engine.GetHost().GetScene().FindEntityByID(owner.Entity))
				return MakeError(ErrorCode::NotFound, "The task owner no longer exists");
			if (!owner.Case.IsNull())
			{
				ENGINE_TRY_ASSIGN(auto caseThread, state.FindReference(owner.Case, ScriptEngine::State::ReferenceKind::Case));
				if (caseThread->Terminal)
					return MakeError(ErrorCode::InvalidState, "A task cannot be created after its test case ended");
			}
			const ScriptExecutionOrigin outerOrigin = SandboxAccess::GetExecutionOrigin(*state.Vm);
			const ScriptExecutionOrigin origin = kind == ScriptThreadKind::Case ? ScriptExecutionOrigin::TestDriver
				: outerOrigin == ScriptExecutionOrigin::Pure                    ? ScriptExecutionOrigin::Gameplay
																				: outerOrigin;
			ScriptReference threadId{};
			const auto operation = [&state, function, owner, kind, origin, &threadId](ScriptCall& call) -> int
			{
				const auto thread = SandboxAccess::CreateThread(call);
				if (!thread)
					return Lua::RaiseError(call, thread.error());
				threadId = state.Retain(call.State, -1, kind == ScriptThreadKind::Case ? ScriptEngine::State::ReferenceKind::Case : ScriptEngine::State::ReferenceKind::Task);
				auto& record = state.References.find(threadId.Index)->second;
				record.Thread = *thread;
				record.Owner = owner;
				record.Origin = origin;
				record.Due = state.Engine.GetHost().GetFrameState().Tick;
				const Status pushed = PushFunction(call, function);
				if (!pushed)
					return Lua::RaiseError(call, pushed.error());
				lua_xmove(call.State, *thread, 1);
				return 0;
			};
			const auto result = state.RunNative(operation, origin, owner, "Task.Create");
			if (!result || result->Failure)
			{
				state.Release(threadId);
				if (!result)
					return std::unexpected(result.error());
				const ScriptError& error = *result->Failure;
				return std::unexpected(Error(error.Kind == ScriptErrorKind::Timeout ? ErrorCode::Timeout : ErrorCode::Script, error.Message)
						.WithLocation({ error.Script, error.Line, error.Column, error.JsonPointer.empty() ? std::nullopt : std::optional<std::string>(error.JsonPointer), error.Entity }));
			}
			return threadId;
		}

		Result<ScriptCaseResume> ScriptEngineAccess::ResumeThread(ScriptEngine& engine, ScriptReference thread)
		{
			return engine.m_State->Resume(thread);
		}

		Result<uint64_t> ScriptEngineAccess::GetDueTick(ScriptEngine& engine, ScriptReference thread)
		{
			engine.m_State->AssertOwner();
			ENGINE_TRY_ASSIGN(auto record, engine.m_State->FindReference(thread, ScriptEngine::State::ReferenceKind::Task));
			if (record->Wait && record->Wait->Kind != ScriptWaitKind::Ticks)
			{
				// Due remains the wait's timeout; predicates, error claims and audio readiness poll every fixed tick.
				const uint64_t now = engine.GetHost().GetFrameState().Tick;
				return now == std::numeric_limits<uint64_t>::max() ? now : now + 1;
			}
			return record->Due;
		}

		Status ScriptEngineAccess::ValidateTask(ScriptEngine& engine, ScriptReference thread)
		{
			engine.m_State->AssertOwner();
			if (thread.IsNull())
				return {};
			if (thread.VMGeneration != engine.GetGeneration())
				return MakeError(ErrorCode::InvalidArgument, "The task belongs to a different script VM");
			const auto found = engine.m_State->References.find(thread.Index);
			if (found != engine.m_State->References.end() && found->second.Kind != ScriptEngine::State::ReferenceKind::Task)
				return MakeError(ErrorCode::InvalidArgument, "The reference is not a task");
			return {};
		}

		void ScriptEngineAccess::CancelThread(ScriptEngine& engine, ScriptReference thread)
		{
			engine.m_State->AssertOwner();
			engine.m_State->Release(thread);
		}

	}

}
