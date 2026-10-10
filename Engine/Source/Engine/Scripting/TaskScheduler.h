#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Scripting/ScriptReference.h"

#include <cstddef>
#include <cstdint>

namespace Engine {

	class ScriptEngine;

	// VM-owned coroutine scheduling in simulation time (§11.3). The engine back-reference outlives this scheduler;
	// no callback captures scene/component references. Main-thread-only. Each task gets a monotonically ordered ID,
	// retained function/thread references and an owner. Foreign/stale/function-of-wrong-kind values never reach Lua.
	class TaskScheduler
	{
	public:
		explicit TaskScheduler(ScriptEngine& engine);
		~TaskScheduler();
		TaskScheduler(const TaskScheduler&) = delete;
		TaskScheduler& operator=(const TaskScheduler&) = delete;

		// Retains the function independently of the caller's reference. Spawn resumes immediately until its first
		// yield/end, inheriting an active outer callback deadline; later resumes receive fresh deadlines. Delay waits
		// ceil(seconds / fixedDelta) ticks, with a minimum of one tick for zero. Invalid/nonfinite delays are rejected.
		[[nodiscard]] Result<ScriptReference> Spawn(ScriptReference function, ScriptTaskOwner owner = {});
		[[nodiscard]] Result<ScriptReference> Delay(double seconds, ScriptReference function, ScriptTaskOwner owner = {});
		// Cancelling a stale/completed task succeeds without touching another task. A live non-task/foreign-VM value
		// is InvalidArgument. Cancellation never resumes code. An executing task terminates at its next safe boundary.
		[[nodiscard]] Status Cancel(ScriptReference task);
		void CancelEntity(UUID entity);
		void CancelCase(ScriptReference caseThread);
		void CancelAll();
		// Stable due-task snapshot, ordered by due tick then creation sequence. Spawned tasks already ran their first
		// slice; they cannot run a second slice in this snapshot. Wait(0)/WaitTicks(0) resume no earlier than next tick.
		// Host frame state is the sole clock. Script faults publish once through ScriptEngine and cancel the owner.
		void ResumeDue();
		[[nodiscard]] size_t GetCount() const;
	private:
		struct State;
		Scope<State> m_State{};
	};

}
