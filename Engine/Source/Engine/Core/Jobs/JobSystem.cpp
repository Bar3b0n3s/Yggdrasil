#include "EnginePCH.h"
#include "Engine/Core/Jobs/JobSystem.h"

#include "Engine/Core/FatalError.h"
#include "Engine/Core/Profiler.h"

#include <algorithm>
#include <exception>
#include <format>
#include <new>
#include <system_error>

// Synchronization: m_Mutex guards the queue, the running-job list and the stop flag. A job's own completion state is
// guarded by its Detail::JobState. Tasks are run, cancelled and destroyed outside m_Mutex, because completing a job
// runs its continuation hook (a post to the MainThreadQueue) and destroying it runs the destructors of its captures.

namespace Engine {

	static void RunJob(Detail::JobTask& task)
	{
		ENGINE_PROFILE_SCOPE("JobSystem.Job");
		task.Run();
	}

	JobSystem::JobSystem(uint32_t workerCount, MainThreadQueue& mainThreadQueue)
		: m_MainThreadQueue(mainThreadQueue), m_WorkerCount(workerCount)
	{
		m_Workers.reserve(workerCount);
		for (uint32_t workerIndex = 0; workerIndex < workerCount; ++workerIndex)
		{
			// std::thread throws only when the OS cannot create another thread, a fatal environment failure while the
			// context initializes (§4.1, §4.6). Unwinding instead would destroy the workers already started while they
			// are joinable, which calls std::terminate.
			try
			{
				m_Workers.emplace_back([this, workerIndex]()
				{
					WorkerMain(workerIndex);
				});
			}
			catch (const std::system_error& error)
			{
				FatalError(FatalErrorKind::InitFailed,
					std::format("Could not start job worker thread {} of {}: {}", workerIndex + 1, workerCount, error.what()));
			}
		}
	}

	JobSystem::~JobSystem()
	{
		std::deque<Scope<Detail::JobTask>> notStarted;
		{
			std::scoped_lock lock(m_Mutex);
			// Joining the worker that runs this destructor would deadlock; in Dist it would hang, so the check stays.
			ENGINE_CORE_VERIFY(!IsRunningJobLocked(), "A JobSystem is destroyed from inside one of its own jobs");
			m_IsStopping = true;
			notStarted.swap(m_Queue);
			m_WorkAvailable.notify_all();
		}

		// Cancelled in submission order. Their continuations are still posted and run at the next Drain.
		for (Scope<Detail::JobTask>& task : notStarted)
		{
			task->Cancel();
			task.reset();
		}

		for (std::thread& worker : m_Workers)
			worker.join();

		// Inline jobs that other threads are still running use this object until they finish.
		std::unique_lock lock(m_Mutex);
		m_Idle.wait(lock, [this]()
		{
			return m_RunningThreads.empty();
		});
	}

	uint32_t JobSystem::GetDefaultWorkerCount()
	{
		const uint32_t hardwareThreads = std::thread::hardware_concurrency();
		return hardwareThreads > 3 ? hardwareThreads - 2 : 1;
	}

	void JobSystem::WaitIdle()
	{
		std::unique_lock lock(m_Mutex);
		// A job waiting for its own system to become idle would wait for itself; in Dist it would hang, so the check
		// stays.
		ENGINE_CORE_VERIFY(!IsRunningJobLocked(), "JobSystem::WaitIdle called from inside one of its own jobs");
		m_Idle.wait(lock, [this]()
		{
			return m_Queue.empty() && m_RunningThreads.empty();
		});
	}

	void JobSystem::Enqueue(Scope<Detail::JobTask> task)
	{
		bool isCancelled = false;
		{
			std::scoped_lock lock(m_Mutex);
			if (m_IsStopping)
			{
				isCancelled = true;
			}
			else if (IsInline())
			{
				m_RunningThreads.push_back(std::this_thread::get_id());
			}
			else
			{
				m_Queue.push_back(std::move(task));
				m_WorkAvailable.notify_one();
				return;
			}
		}

		if (isCancelled)
		{
			task->Cancel();
			return;
		}

		// Inline mode: the job runs on the submitting thread before Submit returns, so jobs run and complete in
		// submission order. An exception propagates to the submitter's boundary; the scope still removes the running
		// entry, so WaitIdle and the destructor never wait for a job that is gone.
		class RunningJobScope
		{
		public:
			explicit RunningJobScope(JobSystem& system)
				: m_System(system)
			{
			}

			~RunningJobScope()
			{
				m_System.FinishJob();
			}

			RunningJobScope(const RunningJobScope&) = delete;
			RunningJobScope& operator=(const RunningJobScope&) = delete;
		private:
			JobSystem& m_System;
		};

		const RunningJobScope running(*this);
		RunJob(*task);
		task.reset(); // the job's captures are released before WaitIdle can return
	}

	void JobSystem::WorkerMain(uint32_t workerIndex)
	{
		// The worker entry is the catch-all boundary of §4.6 item 6. Jobs report failures through their Result and
		// never throw, so an exception here is a bug or an exhausted environment: it ends the process instead of
		// unwinding into std::thread, which would call std::terminate without a report.
		try
		{
			Profiler::SetThreadName(std::format("Job {}", workerIndex + 1));
			for (;;)
			{
				Scope<Detail::JobTask> task;
				{
					std::unique_lock lock(m_Mutex);
					m_WorkAvailable.wait(lock, [this]()
					{
						return m_IsStopping || !m_Queue.empty();
					});
					// The destructor empties the queue when it sets the stop flag, and later jobs are cancelled, so an
					// empty queue here means the system is shutting down.
					if (m_Queue.empty())
						return;

					task = std::move(m_Queue.front());
					m_Queue.pop_front();
					m_RunningThreads.push_back(std::this_thread::get_id());
				}

				RunJob(*task);
				task.reset(); // the job's captures are released before WaitIdle can return
				FinishJob();
			}
		}
		catch (const std::bad_alloc&)
		{
			FatalError(FatalErrorKind::OutOfMemory, "A job ran out of memory");
		}
		catch (const std::exception& exception)
		{
			FatalError(FatalErrorKind::UnhandledException, std::format("A job threw an exception: {}", exception.what()));
		}
		catch (...)
		{
			FatalError(FatalErrorKind::UnhandledException, "A job threw an exception of unknown type");
		}
	}

	void JobSystem::FinishJob()
	{
		std::scoped_lock lock(m_Mutex);
		const auto entry = std::ranges::find(m_RunningThreads, std::this_thread::get_id());
		ENGINE_CORE_ASSERT(entry != m_RunningThreads.end(), "A finished job was not registered as running");
		if (entry != m_RunningThreads.end())
			m_RunningThreads.erase(entry);
		if (m_Queue.empty() && m_RunningThreads.empty())
			m_Idle.notify_all();
	}

	bool JobSystem::IsRunningJobLocked() const
	{
		return std::ranges::find(m_RunningThreads, std::this_thread::get_id()) != m_RunningThreads.end();
	}

}
