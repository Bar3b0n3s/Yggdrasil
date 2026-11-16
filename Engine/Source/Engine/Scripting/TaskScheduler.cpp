#include "EnginePCH.h"
#include "Engine/Scripting/TaskScheduler.h"

#include "Engine/Core/Assert.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptTime.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <thread>
#include <vector>

namespace Engine {

	struct TaskScheduler::State
	{
		struct Task
		{
			ScriptReference Thread{};
			ScriptTaskOwner Owner{};
			uint64_t Due = 0;
			// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 2): Due counts the world's ticks, which hold while the
			// world is paused (ScriptEngineAccess::UsesWorldClock); otherwise the session's.
			bool WorldClock = false;
		};

		// The current tick of a task's clock (the host's session tick, or the world's: Tick - WorldHeldTicks).
		[[nodiscard]] static uint64_t ClockTick(const ScriptFrameState& frame, bool worldClock)
		{
			return worldClock ? frame.Tick - std::min(frame.WorldHeldTicks, frame.Tick) : frame.Tick;
		}

		explicit State(ScriptEngine& engine)
			: Engine(engine)
		{
		}
		void AssertOwner() const
		{
			ENGINE_CORE_ASSERT(OwnerThread == std::this_thread::get_id(), "TaskScheduler must stay on its owner thread");
		}

		void Resume(ScriptReference thread)
		{
			const auto scheduled = Tasks.find(thread.Index);
			if (scheduled == Tasks.end())
				return;
			const ScriptTaskOwner owner = scheduled->second.Owner;
			const auto result = Detail::ScriptEngineAccess::ResumeThread(Engine, thread);
			if (!result && result.error().GetCode() != ErrorCode::Cancelled)
				Detail::ScriptEngineAccess::PublishTaskFailure(Engine, result.error(), owner);
			// Re-find after user code, which may cancel this task or its entire owner.
			const auto current = Tasks.find(thread.Index);
			if (current == Tasks.end())
				return;
			if (!result || result->State != ScriptCaseState::Yielded)
			{
				Detail::ScriptEngineAccess::CancelThread(Engine, thread);
				Tasks.erase(current);
				return;
			}
			const auto due = Detail::ScriptEngineAccess::GetDueTick(Engine, thread);
			if (!due)
			{
				Detail::ScriptEngineAccess::CancelThread(Engine, thread);
				Tasks.erase(current);
				Detail::ScriptEngineAccess::PublishTaskFailure(Engine, due.error(), owner);
				return;
			}
			current->second.Due = *due;
		}

		ScriptEngine& Engine; // enclosing owner, outlives this scheduler
		std::thread::id OwnerThread = std::this_thread::get_id();
		std::map<uint64_t, Task> Tasks{};
	};

	TaskScheduler::TaskScheduler(ScriptEngine& engine)
		: m_State(CreateScope<State>(engine))
	{
	}

	TaskScheduler::~TaskScheduler()
	{
		CancelAll();
	}

	Result<ScriptReference> TaskScheduler::Spawn(ScriptReference function, ScriptTaskOwner owner)
	{
		m_State->AssertOwner();
		ENGINE_TRY_ASSIGN(auto thread, Detail::ScriptEngineAccess::CreateThread(m_State->Engine, function, owner, Detail::ScriptThreadKind::Task));
		const bool worldClock = Detail::ScriptEngineAccess::UsesWorldClock(m_State->Engine, thread);
		m_State->Tasks.emplace(thread.Index, State::Task{ thread, owner, State::ClockTick(m_State->Engine.GetHost().GetFrameState(), worldClock), worldClock });
		m_State->Resume(thread);
		return thread;
	}

	Result<ScriptReference> TaskScheduler::Delay(double seconds, ScriptReference function, ScriptTaskOwner owner)
	{
		m_State->AssertOwner();
		const ScriptFrameState frame = m_State->Engine.GetHost().GetFrameState();
		if (!std::isfinite(seconds) || seconds < 0.0 || !std::isfinite(frame.FixedDeltaTime) || frame.FixedDeltaTime <= 0.0)
			return MakeError(ErrorCode::InvalidArgument, "Task.Delay requires finite nonnegative seconds and a positive fixed delta");
		// Decimal-safe, as Task.Wait: 0.14 s at 50 Hz is 7 ticks, not 8 (ScriptTime.h).
		const double delay = std::max(1.0, ScriptSecondsToTicks(seconds, frame.FixedDeltaTime));
		// 2^64 is exactly representable; converting it or infinity to uint64_t would be undefined.
		if (!std::isfinite(delay) || delay >= 18446744073709551616.0)
			return MakeError(ErrorCode::InvalidArgument, "Task.Delay exceeds the simulation tick range");
		const uint64_t ticks = static_cast<uint64_t>(delay);
		if (ticks > std::numeric_limits<uint64_t>::max() - frame.Tick)
			return MakeError(ErrorCode::InvalidArgument, "Task.Delay exceeds the simulation tick range");
		ENGINE_TRY_ASSIGN(auto thread, Detail::ScriptEngineAccess::CreateThread(m_State->Engine, function, owner, Detail::ScriptThreadKind::Task));
		// The delay counts the task's clock: a world behaviour's holds while the world is paused (M35, decision 2).
		const bool worldClock = Detail::ScriptEngineAccess::UsesWorldClock(m_State->Engine, thread);
		m_State->Tasks.emplace(thread.Index, State::Task{ thread, owner, State::ClockTick(frame, worldClock) + ticks, worldClock });
		return thread;
	}

	Status TaskScheduler::Cancel(ScriptReference task)
	{
		m_State->AssertOwner();
		ENGINE_TRY(Detail::ScriptEngineAccess::ValidateTask(m_State->Engine, task));
		Detail::ScriptEngineAccess::CancelThread(m_State->Engine, task);
		m_State->Tasks.erase(task.Index);
		return {};
	}

	void TaskScheduler::CancelInstance(UUID entity, uint32_t slot)
	{
		m_State->AssertOwner();
		for (auto current = m_State->Tasks.begin(); current != m_State->Tasks.end();)
		{
			if (current->second.Owner.Entity != entity || current->second.Owner.Slot != slot)
			{
				++current;
				continue;
			}
			Detail::ScriptEngineAccess::CancelThread(m_State->Engine, current->second.Thread);
			current = m_State->Tasks.erase(current);
		}
	}

	void TaskScheduler::CancelCase(ScriptReference caseThread)
	{
		m_State->AssertOwner();
		for (auto current = m_State->Tasks.begin(); current != m_State->Tasks.end();)
		{
			if (current->second.Owner.Case != caseThread)
			{
				++current;
				continue;
			}
			Detail::ScriptEngineAccess::CancelThread(m_State->Engine, current->second.Thread);
			current = m_State->Tasks.erase(current);
		}
	}

	void TaskScheduler::CancelAll()
	{
		m_State->AssertOwner();
		for (const auto& [index, task] : m_State->Tasks)
		{
			static_cast<void>(index);
			Detail::ScriptEngineAccess::CancelThread(m_State->Engine, task.Thread);
		}
		m_State->Tasks.clear();
	}

	void TaskScheduler::ResumeDue()
	{
		m_State->AssertOwner();
		const ScriptFrameState frame = m_State->Engine.GetHost().GetFrameState();
		std::vector<State::Task> due;
		for (const auto& [index, task] : m_State->Tasks)
		{
			static_cast<void>(index);
			// M35 (decision 2): while the world is paused its behaviours' tasks hold, whatever their due tick.
			if (task.WorldClock && frame.WorldPaused)
				continue;
			if (task.Due <= State::ClockTick(frame, task.WorldClock))
				due.push_back(task);
		}
		std::sort(due.begin(), due.end(), [](const State::Task& left, const State::Task& right)
		{
			return left.Due != right.Due ? left.Due < right.Due : left.Thread.Index < right.Thread.Index;
		});
		for (const State::Task& task : due)
		{
			if (m_State->Tasks.contains(task.Thread.Index))
				m_State->Resume(task.Thread);
		}
	}

	size_t TaskScheduler::GetCount() const
	{
		m_State->AssertOwner();
		return m_State->Tasks.size();
	}

}
