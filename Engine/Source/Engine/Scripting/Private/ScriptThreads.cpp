#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/SandboxAccess.h"

#include <algorithm>
#include <array>
#include <limits>

namespace Engine {

	namespace Utils {

		static ScriptError ThreadFailure(const Error& error, ScriptTaskOwner owner, std::string_view callback)
		{
			ScriptError result{};
			result.Kind = error.GetCode() == ErrorCode::Timeout ? ScriptErrorKind::Timeout : ScriptErrorKind::Runtime;
			result.Message = error.ToString();
			result.Callback = callback;
			result.Entity = owner.Entity;
			const auto& location = error.GetLocation();
			result.Script = location.File;
			result.Line = location.Line;
			result.Column = location.Column;
			result.JsonPointer = location.JsonPointer.value_or("");
			return result;
		}

		// Validate the complete pattern before installing a wait, including suffixes string.find might never visit.
		Status ValidateErrorPattern(std::string_view pattern)
		{
			std::array<bool, 32> closed{};
			std::vector<size_t> open;
			size_t captures = 0;
			for (size_t index = 0; index < pattern.size(); ++index)
			{
				char item = pattern[index];
				if (item == '%')
				{
					if (++index == pattern.size())
						return MakeError(ErrorCode::Script, "Malformed error pattern: trailing percent");
					item = pattern[index];
					if (item == 'b')
					{
						if (pattern.size() - index <= 2)
							return MakeError(ErrorCode::Script, "Malformed error pattern: %b needs two delimiters");
						index += 2;
						continue;
					}
					if (item >= '0' && item <= '9')
					{
						const size_t capture = static_cast<size_t>(item - '0');
						if (capture == 0 || capture > captures || !closed[capture - 1])
							return MakeError(ErrorCode::Script, "Malformed error pattern: invalid capture reference");
						continue;
					}
					if (item != 'f')
						continue;
					if (++index == pattern.size() || pattern[index] != '[')
						return MakeError(ErrorCode::Script, "Malformed error pattern: %f needs a character class");
					item = '[';
				}
				if (item == '[')
				{
					++index;
					if (index < pattern.size() && pattern[index] == '^')
						++index;
					// A closing bracket in the first slot is a literal character of the class.
					bool first = true;
					for (; index < pattern.size(); ++index)
					{
						if (pattern[index] == ']' && !first)
							break;
						first = false;
						if (pattern[index] == '%' && index + 1 < pattern.size())
							++index;
					}
					if (index == pattern.size())
						return MakeError(ErrorCode::Script, "Malformed error pattern: missing closing bracket");
				}
				else if (item == '(')
				{
					if (captures == closed.size())
						return MakeError(ErrorCode::Script, "Malformed error pattern: too many captures");
					const size_t capture = captures++;
					if (index + 1 < pattern.size() && pattern[index + 1] == ')')
					{
						closed[capture] = true;
						++index;
					}
					else
						open.push_back(capture);
				}
				else if (item == ')')
				{
					if (open.empty())
						return MakeError(ErrorCode::Script, "Malformed error pattern: unmatched closing capture");
					closed[open.back()] = true;
					open.pop_back();
				}
			}
			if (!open.empty())
				return MakeError(ErrorCode::Script, "Malformed error pattern: unfinished capture");
			return {};
		}

	}

	bool ScriptEngine::State::MatchError(ScriptCall& call, std::string_view pattern, std::string_view message)
	{
		lua_getglobal(call.State, "string");
		lua_getfield(call.State, -1, "find");
		lua_remove(call.State, -2);
		Lua::PushString(call, message);
		Lua::PushString(call, pattern);
		const auto result = Lua::ProtectedCall(call, 2, 1);
		if (!result)
			Lua::RaiseError(call, result.error());
		if (result->Failure)
			Lua::RaiseError(call, result->Failure->Message);
		const bool matched = !lua_isnil(call.State, -1);
		lua_pop(call.State, 1);
		return matched;
	}

	bool ScriptEngine::State::ClaimError(ScriptCall& call, ScriptReference caseThread, std::string_view pattern)
	{
		for (auto& observed : ObservedErrors)
		{
			if (observed.Case == caseThread && observed.Claimable && !observed.Claimed && MatchError(call, pattern, observed.Error.Message))
			{
				observed.Claimed = true;
				TestHost->OnExpectedScriptError(observed.Error);
				return true;
			}
		}
		return false;
	}

	Result<int> ScriptEngine::State::PrepareWait(ScriptCall& call, const Detail::ScriptTestWait& wait)
	{
		Reference* thread = FindThread(call.State);
		if (thread == nullptr || !thread->Running || !lua_isyieldable(call.State))
			return MakeError(ErrorCode::Script, "Task.Wait can only be used inside Task.Spawn, Task.Delay or a Test.Case body");
		if (thread->Wait.has_value())
			return MakeError(ErrorCode::InvalidState, "This thread already has a pending wait");
		const uint64_t now = Engine.GetHost().GetFrameState().Tick;
		const uint64_t delay = wait.Kind == Detail::ScriptWaitKind::Ticks ? std::max(uint64_t{ 1 }, wait.Ticks) : wait.Ticks;
		if (delay > std::numeric_limits<uint64_t>::max() - now)
			return MakeError(ErrorCode::InvalidArgument, "The wait exceeds the simulation tick range");
		if (wait.Kind == Detail::ScriptWaitKind::ExpectedError)
		{
			if (thread->Owner.Case.IsNull())
				return MakeError(ErrorCode::InvalidState, "Test.ExpectScriptError requires an active case");
			ENGINE_TRY(Utils::ValidateErrorPattern(wait.Pattern));
			if (ClaimError(call, thread->Owner.Case, wait.Pattern))
				return 0;
		}
		ScriptReference predicate{};
		if (wait.Kind == Detail::ScriptWaitKind::Predicate)
		{
			ENGINE_TRY(Detail::ScriptEngineAccess::PushFunction(call, wait.Predicate));
			const auto result = Lua::ProtectedCall(call, 0, 1);
			if (!result || result->Failure)
			{
				if (!result)
					return std::unexpected(result.error());
				return MakeError(ErrorCode::Script, "{}", result->Failure->Message);
			}
			// Return through the binding so it can release its own retained function, even for malformed results.
			if (lua_type(call.State, -1) != LUA_TBOOLEAN)
			{
				lua_pop(call.State, 1);
				return MakeError(ErrorCode::Script, "Test.WaitUntil predicate must return a boolean");
			}
			const bool ready = lua_toboolean(call.State, -1) != 0;
			lua_pop(call.State, 1);
			if (ready)
				return 0;
			ENGINE_TRY(Detail::ScriptEngineAccess::PushFunction(call, wait.Predicate));
			predicate = Retain(call.State, -1, ReferenceKind::Function);
			lua_pop(call.State, 1);
		}
		if (wait.Kind == Detail::ScriptWaitKind::Audio)
		{
			ENGINE_TRY(call.CheckWritable());
			if (wait.Ticks == 0 || wait.Ticks > std::numeric_limits<uint32_t>::max())
				return MakeError(ErrorCode::InvalidArgument, "Audio capture needs a positive bounded tick count");
			ENGINE_TRY(TestHost->BeginAudioCapture(static_cast<uint32_t>(wait.Ticks)));
		}
		thread->Wait = wait;
		thread->Wait->Predicate = predicate;
		thread->Due = now + delay;
		return -1;
	}

	Result<ScriptCaseResume> ScriptEngine::State::Resume(ScriptReference id)
	{
		AssertOwner();
		if (id.IsNull() || id.VMGeneration != Engine.GetGeneration())
			return MakeError(ErrorCode::InvalidArgument, "The thread belongs to a different script VM");
		auto found = References.find(id.Index);
		if (found == References.end() || found->second.Kind == ReferenceKind::Function || found->second.Cancelled || found->second.Running)
			return MakeError(ErrorCode::InvalidArgument, "The thread is released, already running or has the wrong kind");
		if (found->second.Terminal)
		{
			DiscardThreadContents(found->second);
			return *found->second.Terminal;
		}
		if (Engine.IsStopped())
			return MakeError(ErrorCode::InvalidState, "The script engine has stopped");
		const ScriptTaskOwner owner = found->second.Owner;
		const bool caseBody = found->second.Kind == ReferenceKind::Case;
		const ScriptExecutionOrigin origin = found->second.Origin;
		lua_State* thread = found->second.Thread;
		int resumeArguments = 0;
		if (found->second.Wait)
		{
			const Detail::ScriptTestWait wait = *found->second.Wait;
			const uint64_t now = Engine.GetHost().GetFrameState().Tick;
			bool ready = wait.Kind == Detail::ScriptWaitKind::Ticks && now >= found->second.Due;
			std::optional<ScriptError> waitFailure;
			if (wait.Kind != Detail::ScriptWaitKind::Ticks)
			{
				// A predicate runs on a protected VM entry rather than on the suspended coroutine. Keep the
				// logical owner alive until it unwinds, even if it cancels itself or ends its owning case.
				struct PollScope
				{
					State& StateOwner;
					ScriptReference Thread{};
					ScriptReference Previous{};
					~PollScope()
					{
						const auto current = StateOwner.References.find(Thread.Index);
						if (current != StateOwner.References.end())
							current->second.Running = false;
						StateOwner.CurrentThread = Previous;
					}
				} poll{ *this, id, CurrentThread };
				CurrentThread = id;
				found->second.Running = true;
				const auto operation = [this, thread, &wait, owner, &ready, &resumeArguments, &waitFailure](ScriptCall& call) -> int
				{
					if (wait.Kind == Detail::ScriptWaitKind::ExpectedError)
						ready = ClaimError(call, owner.Case, wait.Pattern);
					else if (wait.Kind == Detail::ScriptWaitKind::Predicate)
					{
						const Status pushed = Detail::ScriptEngineAccess::PushFunction(call, wait.Predicate);
						if (!pushed)
							return Lua::RaiseError(call, pushed.error());
						const auto result = Lua::ProtectedCall(call, 0, 1);
						if (!result)
							return Lua::RaiseError(call, result.error());
						if (result->Failure)
							waitFailure = result->Failure;
						else
						{
							ready = Lua::Check<bool>(call, -1);
							lua_pop(call.State, 1);
						}
					}
					else if (wait.Kind == Detail::ScriptWaitKind::Audio && TestHost->IsAudioCaptureReady())
					{
						const auto levels = TestHost->EndAudioCapture();
						if (!levels)
							return Lua::RaiseError(call, levels.error());
						lua_createtable(call.State, 0, 3);
						Lua::Push(call, levels->RmsLeft);
						lua_setfield(call.State, -2, "RmsLeft");
						Lua::Push(call, levels->RmsRight);
						lua_setfield(call.State, -2, "RmsRight");
						Lua::Push(call, levels->Peak);
						lua_setfield(call.State, -2, "Peak");
						lua_xmove(call.State, thread, 1);
						resumeArguments = 1;
						ready = true;
					}
					return 0;
				};
				const auto result = RunNative(operation, origin, owner, "Test.Wait");
				if (!result)
					waitFailure = Utils::ThreadFailure(result.error(), owner, "Test.Wait");
				else if (result->Failure)
					waitFailure = result->Failure;
			}
			found = References.find(id.Index);
			if (found == References.end())
				return MakeError(ErrorCode::Cancelled, "The waiting thread was cancelled");
			const bool safetyFailure = waitFailure && (waitFailure->Kind == ScriptErrorKind::Memory || waitFailure->Kind == ScriptErrorKind::Timeout);
			const auto caseRecord = References.find(owner.Case.Index);
			if (!safetyFailure && !owner.Case.IsNull() && caseRecord != References.end() && caseRecord->second.Terminal)
			{
				const ScriptCaseResume terminal = *caseRecord->second.Terminal;
				found->second.Terminal = terminal;
				DiscardThreadContents(found->second);
				if (found->second.Cancelled)
					Release(id);
				return terminal;
			}
			if (!safetyFailure && found->second.Cancelled)
			{
				Release(id);
				return MakeError(ErrorCode::Cancelled, "The waiting thread was cancelled");
			}
			if (waitFailure)
			{
				ScriptCaseResume failed{};
				failed.State = ScriptCaseState::Failed;
				failed.Message = waitFailure->Message;
				failed.File = waitFailure->Script;
				failed.Line = waitFailure->Line;
				failed.Error = waitFailure;
				found->second.Terminal = failed;
				// A suspended thread still owns its stack, including any predicate closure's captured heap.
				// Drop that root before Publish performs the first memory breach's full collection.
				DiscardThreadContents(found->second);
				if (found->second.Cancelled)
					Release(id);
				Publish(*waitFailure, owner, caseBody);
				return failed;
			}
			if (!ready)
			{
				ScriptCaseResume waiting{};
				waiting.File = wait.Location.File;
				waiting.Line = wait.Location.Line;
				if ((wait.Kind == Detail::ScriptWaitKind::Predicate || wait.Kind == Detail::ScriptWaitKind::ExpectedError)
					&& now >= found->second.Due)
				{
					waiting.State = ScriptCaseState::Failed;
					waiting.Message = wait.Kind == Detail::ScriptWaitKind::Predicate ? "Test.WaitUntil exceeded its tick limit"
																					 : "No matching script error was observed within the tick limit";
					found->second.Terminal = waiting;
					DiscardThreadContents(found->second);
					if (!owner.Case.IsNull() && caseRecord != References.end())
						caseRecord->second.Terminal = waiting;
					TestHost->Report({ ScriptTestSignal::Fail, waiting.Message, waiting.File, waiting.Line });
					if (!owner.Case.IsNull())
						Scheduler->CancelCase(owner.Case);
				}
				return waiting;
			}
			Release(wait.Predicate);
			found->second.Wait.reset();
		}
		else if (found->second.Started && Engine.GetHost().GetFrameState().Tick < found->second.Due)
			return ScriptCaseResume{};

		const auto resumeProtected = [this, id, owner, origin, thread, caseBody, resumeArguments]() -> Result<ScriptCallResult>
		{
			ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*Vm, origin));
			struct ResumeScope
			{
				State& StateOwner;
				ScriptReference Thread{};
				ScriptTaskOwner PreviousOwner{};
				lua_State* PreviousThread = nullptr;
				ScriptReference PreviousLogicalThread{};
				bool HasContext = false;
				~ResumeScope()
				{
					const auto current = StateOwner.References.find(Thread.Index);
					if (current != StateOwner.References.end())
						current->second.Running = false;
					StateOwner.CurrentOwner = PreviousOwner;
					StateOwner.ActiveThread = PreviousThread;
					StateOwner.CurrentThread = PreviousLogicalThread;
					if (HasContext)
						Detail::SandboxAccess::PopContext(*StateOwner.Vm);
					Detail::SandboxAccess::LeaveProtected(*StateOwner.Vm);
				}
			} scope{ *this, id, CurrentOwner, ActiveThread, CurrentThread, false };
			lua_State* from = ActiveThread;
			CurrentOwner = owner;
			ActiveThread = thread;
			CurrentThread = id;
			auto& record = References.find(id.Index)->second;
			record.Running = true;
			record.Started = true;
			Detail::SandboxExecutionContext context{};
			context.Callback = caseBody ? "Test.Case" : "Task";
			context.Entity = owner.Entity;
			context.Tick = Engine.GetHost().GetFrameState().Tick;
			const auto instance = Instances.find(owner.Entity);
			if (instance != Instances.end())
			{
				context.EntityName = instance->second.Name;
				if (instance->second.Script != nullptr)
					context.Script = instance->second.Script->SourceMap.Path;
			}
			ENGINE_TRY(Detail::SandboxAccess::PushContext(*Vm, context));
			scope.HasContext = true;
			ENGINE_TRY_ASSIGN(auto call, Detail::SandboxAccess::ThreadCall(*Vm, thread, context.Callback));
			return Lua::ProtectedResume(call, from, resumeArguments);
		};
		auto resumed = resumeProtected();
		found = References.find(id.Index);
		if (found == References.end())
			return MakeError(ErrorCode::Cancelled, "The running thread was released");
		if (!resumed)
		{
			ScriptCallResult failure{};
			failure.Failure = Utils::ThreadFailure(resumed.error(), owner, caseBody ? "Test.Case" : "Task");
			resumed = std::move(failure);
		}
		const bool safetyFailure = resumed->Failure && (resumed->Failure->Kind == ScriptErrorKind::Memory || resumed->Failure->Kind == ScriptErrorKind::Timeout);
		const auto caseRecord = References.find(owner.Case.Index);
		if (!safetyFailure && !owner.Case.IsNull() && caseRecord != References.end() && caseRecord->second.Terminal)
		{
			const ScriptCaseResume terminal = *caseRecord->second.Terminal;
			found->second.Terminal = terminal;
			DiscardThreadContents(found->second);
			if (found->second.Cancelled)
				Release(id);
			return terminal;
		}
		if (!safetyFailure && found->second.Cancelled)
		{
			Release(id);
			ScriptCaseResume cancelled{};
			cancelled.State = ScriptCaseState::Completed;
			return cancelled;
		}
		if (resumed->Failure)
		{
			ScriptCaseResume failed{};
			failed.State = ScriptCaseState::Failed;
			failed.Message = resumed->Failure->Message;
			failed.File = resumed->Failure->Script;
			failed.Line = resumed->Failure->Line;
			failed.Error = resumed->Failure;
			found->second.Terminal = failed;
			// Release the failed coroutine's reachable heap before the first memory breach's full collection.
			DiscardThreadContents(found->second);
			if (found->second.Cancelled)
				Release(id);
			Publish(*resumed->Failure, owner, caseBody);
			return failed;
		}
		if (!resumed->Yielded)
		{
			ScriptCaseResume completed{};
			completed.State = ScriptCaseState::Completed;
			found->second.Terminal = completed;
			DiscardThreadContents(found->second);
			return completed;
		}
		if (!found->second.Wait)
		{
			const uint64_t now = Engine.GetHost().GetFrameState().Tick;
			found->second.Due = now == std::numeric_limits<uint64_t>::max() ? now : now + 1;
		}
		ScriptCaseResume waiting{};
		if (found->second.Wait)
		{
			waiting.File = found->second.Wait->Location.File;
			waiting.Line = found->second.Wait->Location.Line;
		}
		return waiting;
	}

}
