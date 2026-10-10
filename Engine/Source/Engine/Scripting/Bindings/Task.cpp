#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/TaskScheduler.h"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		static int StopTaskEntry(ScriptCall& call, const Error& error)
		{
			// A yield bypasses pcall's error handler and the terminal/cancelled owner is never resumed again.
			if (lua_isyieldable(call.State))
				return lua_yield(call.State, 0);
			return Lua::RaiseError(call, error);
		}

		static TaskScheduler& BindingScheduler(ScriptCall& call)
		{
			auto* scheduler = call.Engine->GetTaskScheduler();
			if (scheduler == nullptr)
				Lua::RaiseError(call, "task scheduler is unavailable");
			return *scheduler;
		}

		static uint64_t TaskTicks(ScriptCall& call, double ticks)
		{
			// Lua numbers cannot distinguish larger adjacent integers. Check before conversion or due-tick arithmetic.
			constexpr double MaxExactInteger = 9007199254740991.0;
			if (!std::isfinite(ticks) || ticks < 0.0 || ticks > MaxExactInteger || std::floor(ticks) != ticks)
				Lua::RaiseError(call, "wait ticks must be an integer in [0, 9007199254740991]");
			const auto count = std::max(uint64_t{ 1 }, static_cast<uint64_t>(ticks));
			if (count > std::numeric_limits<uint64_t>::max() - call.Engine->GetHost().GetFrameState().Tick)
				Lua::RaiseError(call, "wait would overflow the simulation tick");
			return count;
		}

		static uint64_t TaskSeconds(ScriptCall& call, double seconds)
		{
			if (seconds < 0.0)
				Lua::RaiseError(call, "wait seconds must be nonnegative");
			const double delta = call.Engine->GetHost().GetFrameState().FixedDeltaTime;
			if (!std::isfinite(delta) || delta <= 0.0)
				Lua::RaiseError(call, "fixed delta must be positive and finite");
			return TaskTicks(call, std::ceil(seconds / delta));
		}

		static int TaskWaitFor(ScriptCall& call, uint64_t ticks)
		{
			const auto permission = call.CheckWritable();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = Detail::ScriptEngineAccess::ArmTaskWait(call, ticks);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return lua_yield(call.State, 0);
		}

		static int TaskWait(ScriptCall& call)
		{
			return TaskWaitFor(call, TaskSeconds(call, Lua::Check<double>(call, 1)));
		}

		static int TaskWaitTicks(ScriptCall& call)
		{
			return TaskWaitFor(call, TaskTicks(call, Lua::Check<double>(call, 1)));
		}

		template<bool Delayed>
		static int TaskStart(ScriptCall& call)
		{
			double seconds = 0.0;
			if constexpr (Delayed)
			{
				seconds = Lua::Check<double>(call, 1);
				static_cast<void>(TaskSeconds(call, seconds));
			}
			auto& scheduler = BindingScheduler(call);
			const auto permission = call.CheckWritable();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto function = Detail::ScriptEngineAccess::RetainFunction(call, Delayed ? 2 : 1);
			if (!function)
				return Lua::RaiseError(call, function.error());
			const auto owner = Detail::ScriptEngineAccess::GetOwner(call);
			const auto task = Delayed ? scheduler.Delay(seconds, *function, owner) : scheduler.Spawn(*function, owner);
			call.Engine->ReleaseReference(*function);
			if (!task)
				return Lua::RaiseError(call, task.error());
			const auto continuing = Detail::ScriptEngineAccess::CheckExecution(call);
			if (!continuing)
				return StopTaskEntry(call, continuing.error());
			Lua::PushTaskHandle(call, *task);
			return 1;
		}

		static int TaskCancel(ScriptCall& call)
		{
			const auto task = Lua::CheckTaskHandle(call, 1);
			const auto permission = call.CheckWritable();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = BindingScheduler(call).Cancel(task);
			if (!result)
				return Lua::RaiseError(call, result.error());
			const auto continuing = Detail::ScriptEngineAccess::CheckExecution(call);
			if (!continuing)
				return StopTaskEntry(call, continuing.error());
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterTask(ScriptApiRegistry& api)
		{
			static_cast<void>(api.Type("TaskHandle", "An opaque coroutine handle owned by one script VM."));
			api.Module("Task", "Caller-owned coroutines scheduled in simulation ticks. All scheduling and waits require writable execution.")
				.Function("Wait", &Utils::TaskWait, "(seconds: number) -> ()", "Yields a task or case for ceil(seconds/fixedDelta) ticks, at least one; invalid in nonyieldable callbacks.")
				.Function("WaitTicks", &Utils::TaskWaitTicks, "(ticks: number) -> ()", "Yields for an exactly representable nonnegative integer number of ticks, at least one.")
				.Function("Spawn", &Utils::TaskStart<false>, "(fn: () -> ()) -> TaskHandle", "Runs immediately to the first yield/end; inherits the caller's entity, case and deadline.")
				.Function("Delay", &Utils::TaskStart<true>, "(seconds: number, fn: () -> ()) -> TaskHandle", "Schedules the caller-owned function after ceil(seconds/fixedDelta) ticks, at least one.")
				.Function("Cancel", &Utils::TaskCancel, "(handle: TaskHandle) -> ()", "Cancels without resuming; cancelling a completed or already cancelled task succeeds.");
			return {};
		}

	}

}
