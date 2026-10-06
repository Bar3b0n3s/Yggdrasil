#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UniqueFunction.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace Engine {

	// Work handed to the main thread (Architecture §4.2 frame step 2, §4.11): job completions, asset swaps and
	// file-watcher results run at the start of the next frame, in the order they were posted. Owned per context by
	// EngineContext. Dropping stale completions (§7.5 "newest wins") is the posting code's job: it checks its own
	// generation inside the task.
	class MainThreadQueue
	{
	public:
		using Task = UniqueFunction<void()>;

		// The constructing thread becomes the queue's main thread.
		MainThreadQueue();
		// Pending tasks are destroyed without running.
		~MainThreadQueue();

		MainThreadQueue(const MainThreadQueue&) = delete;
		MainThreadQueue& operator=(const MainThreadQueue&) = delete;

		// Appends `task` (non-empty, asserted). Thread-safe; callable from any thread, including from a task during
		// Drain.
		void Post(Task task);

		// Runs, on the main thread (asserted), every task that was posted before this call started, in posting order
		// (FIFO across all posting threads, by the order Post acquired the queue). Tasks posted while draining run at the
		// next Drain, so a task that re-posts itself cannot starve the frame. Returns the number of tasks run. Must not
		// be called from inside a task.
		uint32_t Drain();

		// The number of tasks waiting. Thread-safe; a snapshot.
		[[nodiscard]] size_t GetPendingCount() const;

		// True on the queue's main thread.
		[[nodiscard]] bool IsMainThread() const;
	private:
		mutable std::mutex m_Mutex; // guards m_Tasks
		std::deque<Task> m_Tasks;
		std::thread::id m_MainThread;
	};

}
