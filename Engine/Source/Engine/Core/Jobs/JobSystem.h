#pragma once

#include "Engine/Core/Assert.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UniqueFunction.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// Background jobs (Architecture §4.11): import, decode, cook, mip generation and file-watcher polling. Jobs take values
// and return a Result; they never touch the ECS, scripts, physics, NVRHI or ImGui (main thread only). Results come back
// through JobHandle::Wait/Take or ContinueOnMainThread.

namespace Engine {

	class JobSystem;

	namespace Detail {

		template<typename T>
		struct IsResultType : std::false_type
		{
		};

		template<typename T>
		struct IsResultType<std::expected<T, Error>> : std::true_type
		{
		};

		// A job: callable without arguments, returning Result<T> for some T (Status for no value).
		template<typename Function>
		concept JobFunction = std::is_invocable_v<std::decay_t<Function>&>
			&& IsResultType<std::invoke_result_t<std::decay_t<Function>&>>::value;

		// The T of a job's Result<T>.
		template<typename Function>
		using JobValue = typename std::invoke_result_t<std::decay_t<Function>&>::value_type;

		// The shared completion state of one job: written once by the job, read by its handles and its continuation.
		template<typename T>
		class JobState
		{
		public:
			// Stores the result (exactly once, asserted), wakes waiters and runs the completion hook, if any, on this
			// thread outside the lock.
			void Complete(Result<T> result)
			{
				UniqueFunction<void()> hook;
				{
					std::scoped_lock lock(m_Mutex);
					ENGINE_CORE_ASSERT(!m_Result.has_value(), "A job completed twice");
					m_Result.emplace(std::move(result));
					hook = std::move(m_OnComplete);
				}
				m_Completed.notify_all();
				if (hook)
					hook();
			}

			// Runs `hook` once the job has completed: right away on this thread if it already has, otherwise on the
			// completing thread. At most one hook per job (asserted).
			void OnComplete(UniqueFunction<void()> hook)
			{
				{
					std::scoped_lock lock(m_Mutex);
					ENGINE_CORE_ASSERT(!m_HasHook, "A job has at most one continuation");
					m_HasHook = true;
					if (!m_Result.has_value())
					{
						m_OnComplete = std::move(hook);
						return;
					}
				}
				hook();
			}

			[[nodiscard]] bool IsComplete() const
			{
				std::scoped_lock lock(m_Mutex);
				return m_Result.has_value();
			}

			[[nodiscard]] const Result<T>& Wait() const
			{
				std::unique_lock lock(m_Mutex);
				m_Completed.wait(lock, [this]()
				{
					return m_Result.has_value();
				});
				ENGINE_CORE_ASSERT(!m_IsTaken, "Reading a job result that was already taken");
				return *m_Result;
			}

			[[nodiscard]] Result<T> Take()
			{
				std::unique_lock lock(m_Mutex);
				m_Completed.wait(lock, [this]()
				{
					return m_Result.has_value();
				});
				ENGINE_CORE_ASSERT(!m_IsTaken, "Taking a job result twice");
				m_IsTaken = true;
				return std::move(*m_Result);
			}
		private:
			mutable std::mutex m_Mutex; // guards every member below
			mutable std::condition_variable m_Completed;
			std::optional<Result<T>> m_Result;
			UniqueFunction<void()> m_OnComplete;
			bool m_HasHook = false;
			bool m_IsTaken = false;
		};

		// The type-erased unit the JobSystem queues: Run executes the job and completes its state; Cancel completes the
		// state with ErrorCode::Cancelled without running it. Exactly one of them is called, once.
		class JobTask
		{
		public:
			virtual ~JobTask() = default;
			virtual void Run() = 0;
			virtual void Cancel() = 0;
		};

		template<typename T, typename Function>
		class TypedJobTask final : public JobTask
		{
		public:
			TypedJobTask(Function&& function, Ref<JobState<T>> state)
				: m_Function(std::move(function)), m_State(std::move(state))
			{
			}

			void Run() override
			{
				m_State->Complete(std::invoke(m_Function));
			}

			void Cancel() override
			{
				m_State->Complete(MakeError(ErrorCode::Cancelled, "the job system shut down before the job started"));
			}
		private:
			Function m_Function;
			Ref<JobState<T>> m_State;
		};

	}

	// The handle of one submitted job. Copyable (copies share the job); a default-constructed handle is invalid.
	// Thread-safe.
	template<typename T>
	class JobHandle
	{
	public:
		JobHandle() = default;

		[[nodiscard]] bool IsValid() const { return m_State != nullptr; }

		// True once the job has completed (successfully, with an error or cancelled). Requires a valid handle (asserted).
		[[nodiscard]] bool IsReady() const
		{
			ENGINE_CORE_ASSERT(IsValid(), "IsReady on an invalid JobHandle");
			return m_State->IsComplete();
		}

		// Blocks until the job has completed and returns its result. Never call it from inside a job (a pool whose workers
		// all wait can deadlock), nor after Take or a ContinueOnMainThread continuation consumed the result (asserted).
		[[nodiscard]] const Result<T>& Wait() const
		{
			ENGINE_CORE_ASSERT(IsValid(), "Wait on an invalid JobHandle");
			return m_State->Wait();
		}

		// Blocks like Wait, then moves the result out. The result can be taken once per job, by any of its handles.
		[[nodiscard]] Result<T> Take()
		{
			ENGINE_CORE_ASSERT(IsValid(), "Take on an invalid JobHandle");
			return m_State->Take();
		}
	private:
		explicit JobHandle(Ref<Detail::JobState<T>> state)
			: m_State(std::move(state))
		{
		}
	private:
		Ref<Detail::JobState<T>> m_State;
	private:
		friend class JobSystem;
	};

	// A pool of worker threads (Architecture §4.11). Owned per context by EngineContext.
	//   - JobSystem(0) is inline mode, for deterministic tests: Submit runs the job on the calling thread before it
	//     returns, so jobs run and complete in submission order ("JobSystem: inline mode preserves submission order").
	//   - With workers, jobs start in submission order (one FIFO queue) and may complete in any order; nothing observable
	//     may depend on completion order (continuations re-establish order through the MainThreadQueue or explicit
	//     sorting).
	//   - A job that throws ends the process: the worker entry is a catch-all boundary that calls
	//     FatalError(FatalErrorKind::UnhandledException) (§4.6 item 6). In inline mode the exception propagates to the
	//     submitting thread's boundary.
	//   - Destruction stops accepting jobs, cancels queued jobs that have not started (their result is
	//     ErrorCode::Cancelled; continuations still get posted), waits for running jobs and joins the workers.
	// Submit, ContinueOnMainThread and WaitIdle are thread-safe; jobs may submit further jobs.
	class JobSystem
	{
	public:
		// `mainThreadQueue` receives ContinueOnMainThread continuations and must outlive the JobSystem (documented
		// back-reference, Architecture §4.7). `workerCount` 0 selects inline mode; GetDefaultWorkerCount() is the
		// production value.
		JobSystem(uint32_t workerCount, MainThreadQueue& mainThreadQueue);
		~JobSystem();

		JobSystem(const JobSystem&) = delete;
		JobSystem& operator=(const JobSystem&) = delete;

		// max(1, std::thread::hardware_concurrency() - 2), with an unknown concurrency counted as 1 (§4.11).
		[[nodiscard]] static uint32_t GetDefaultWorkerCount();

		// Queues `job` (a callable returning Result<T>, moved into the job system) and returns its handle. Capture by value
		// or move: the job may run after the caller's frame is gone.
		template<typename Function>
			requires Detail::JobFunction<Function>
		[[nodiscard]] JobHandle<Detail::JobValue<Function>> Submit(Function&& job)
		{
			using Value = Detail::JobValue<Function>;
			Ref<Detail::JobState<Value>> state = CreateRef<Detail::JobState<Value>>();
			Enqueue(CreateScope<Detail::TypedJobTask<Value, std::decay_t<Function>>>(std::decay_t<Function>(std::forward<Function>(job)),
				state));
			return JobHandle<Value>(std::move(state));
		}

		// Posts `continuation(Result<T>)` to the MainThreadQueue once the job has completed (immediately if it already
		// has), so it runs at the next MainThreadQueue::Drain. The result is moved into the continuation. One continuation
		// per job (asserted); afterwards the handles must not Wait or Take.
		template<typename T, typename Continuation>
			requires std::is_invocable_v<std::decay_t<Continuation>&, Result<T>>
		void ContinueOnMainThread(const JobHandle<T>& handle, Continuation&& continuation)
		{
			ENGINE_CORE_ASSERT(handle.IsValid(), "ContinueOnMainThread on an invalid JobHandle");
			Ref<Detail::JobState<T>> state = handle.m_State;
			MainThreadQueue* queue = &m_MainThreadQueue;
			// Distinct capture names: GCC's -Wshadow reports an init-capture that reuses an enclosing name.
			std::decay_t<Continuation> movedContinuation(std::forward<Continuation>(continuation));
			state->OnComplete([queue, state, ownedContinuation = std::move(movedContinuation)]() mutable
			{
				queue->Post([state, postedContinuation = std::move(ownedContinuation)]() mutable
				{
					postedContinuation(state->Take());
				});
			});
		}

		// Blocks until every job submitted so far has completed. Not from inside a job.
		void WaitIdle();

		[[nodiscard]] uint32_t GetWorkerCount() const { return m_WorkerCount; }
		[[nodiscard]] bool IsInline() const { return m_WorkerCount == 0; }
	private:
		// Inline mode runs the task now; otherwise it joins the FIFO queue. After destruction began, the task is
		// cancelled instead.
		void Enqueue(Scope<Detail::JobTask> task);
		void WorkerMain();
	private:
		MainThreadQueue& m_MainThreadQueue;
		uint32_t m_WorkerCount = 0;
		std::mutex m_Mutex; // guards m_Queue and the workers' bookkeeping
		std::condition_variable m_WorkAvailable;
		std::condition_variable m_Idle;
		std::deque<Scope<Detail::JobTask>> m_Queue;
		std::vector<std::thread> m_Workers;
	};

}
