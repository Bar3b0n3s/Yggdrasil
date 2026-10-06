#include "EnginePCH.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Profiler.h"

namespace Engine {

	MainThreadQueue::MainThreadQueue()
		: m_MainThread(std::this_thread::get_id())
	{
	}

	MainThreadQueue::~MainThreadQueue() = default;

	void MainThreadQueue::Post(Task task)
	{
		ENGINE_CORE_ASSERT(static_cast<bool>(task), "MainThreadQueue::Post needs a non-empty task");

		std::scoped_lock lock(m_Mutex);
		m_Tasks.push_back(std::move(task));
	}

	uint32_t MainThreadQueue::Drain()
	{
		ENGINE_CORE_ASSERT(IsMainThread(), "MainThreadQueue::Drain called off the main thread");
		ENGINE_CORE_ASSERT(!m_IsDraining, "MainThreadQueue::Drain called from inside a task");
		ENGINE_PROFILE_SCOPE("MainThreadQueue.Drain");

		// Taking the whole queue at once fixes the batch: tasks posted from now on, including by the tasks below, land
		// in the emptied m_Tasks and wait for the next Drain.
		std::deque<Task> batch;
		{
			std::scoped_lock lock(m_Mutex);
			batch.swap(m_Tasks);
		}

		// A task that throws ends the frame at the process boundary (§4.6); the flag is still reset on the way out.
		struct DrainingScope
		{
			explicit DrainingScope(bool& isDraining)
				: IsDraining(isDraining)
			{
				IsDraining = true;
			}

			~DrainingScope()
			{
				IsDraining = false;
			}

			DrainingScope(const DrainingScope&) = delete;
			DrainingScope& operator=(const DrainingScope&) = delete;

			bool& IsDraining;
		};
		const DrainingScope draining(m_IsDraining);

		size_t ranCount = 0;
		while (!batch.empty())
		{
			// Each task is destroyed right after it ran, so what it captured is released in posting order too.
			Task task = std::move(batch.front());
			batch.pop_front();
			task();
			++ranCount;
		}
		return static_cast<uint32_t>(std::min<size_t>(ranCount, std::numeric_limits<uint32_t>::max()));
	}

	size_t MainThreadQueue::GetPendingCount() const
	{
		std::scoped_lock lock(m_Mutex);
		return m_Tasks.size();
	}

	bool MainThreadQueue::IsMainThread() const
	{
		return std::this_thread::get_id() == m_MainThread;
	}

}
