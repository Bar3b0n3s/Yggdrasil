#include "TestsPCH.h"

#include "Engine/Core/Jobs/JobSystem.h"

#include <atomic>
#include <latch>
#include <thread>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("JobSystem: inline mode preserves submission order" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			CHECK(jobs.IsInline());

			std::vector<int> started;
			std::vector<int> completed;
			std::vector<JobHandle<int>> handles;
			for (int index = 0; index < 10; ++index)
			{
				handles.push_back(jobs.Submit([&started, index]() -> Result<int>
				{
					started.push_back(index);
					return index * index;
				}));
				CHECK(handles.back().IsReady()); // inline jobs complete before Submit returns
				jobs.ContinueOnMainThread(handles.back(), [&completed](Result<int> result)
				{
					REQUIRE(result.has_value());
					completed.push_back(*result);
				});
			}

			CHECK(started == std::vector<int>{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 });
			CHECK(completed.empty()); // continuations wait for the main thread
			CHECK(queue.Drain() == 10);
			CHECK(completed == std::vector<int>{ 0, 1, 4, 9, 16, 25, 36, 49, 64, 81 });
		}

		TEST_CASE("JobSystem: Wait returns the result and Take moves it out" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(2, queue);

			JobHandle<std::string> handle = jobs.Submit([]() -> Result<std::string>
			{
				return std::string("cooked");
			});
			REQUIRE(handle.IsValid());
			REQUIRE(handle.Wait().has_value());
			CHECK(*handle.Wait() == "cooked");

			const Result<std::string> taken = handle.Take();
			REQUIRE(taken.has_value());
			CHECK(*taken == "cooked");

			const JobHandle<int> invalid;
			CHECK_FALSE(invalid.IsValid());
		}

		TEST_CASE("JobSystem: errors come back as results" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(1, queue);

			JobHandle<void> failing = jobs.Submit([]() -> Status
			{
				return MakeError(ErrorCode::ImportFailed, "corrupt PNG");
			});
			const Status status = failing.Take();
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(status.error().GetMessageText() == "corrupt PNG");
		}

		TEST_CASE("JobSystem: continuations run on the main thread at the next Drain" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(2, queue);

			std::atomic<bool> ranOnMainThread = false;
			std::atomic<int> continuations = 0;
			JobHandle<int> handle = jobs.Submit([]() -> Result<int>
			{
				return 7;
			});
			jobs.ContinueOnMainThread(handle, [&queue, &ranOnMainThread, &continuations](Result<int> result)
			{
				ranOnMainThread = queue.IsMainThread() && result.has_value() && *result == 7;
				++continuations;
			});

			jobs.WaitIdle();
			CHECK(continuations == 0);
			CHECK(queue.Drain() == 1);
			CHECK(continuations == 1);
			CHECK(ranOnMainThread);
		}

		TEST_CASE("JobSystem: worker mode completes every job" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(4, queue);
			CHECK(jobs.GetWorkerCount() == 4);

			std::vector<JobHandle<uint64_t>> handles;
			for (uint64_t index = 1; index <= 1000; ++index)
			{
				handles.push_back(jobs.Submit([index]() -> Result<uint64_t>
				{
					return index;
				}));
			}

			uint64_t sum = 0;
			for (JobHandle<uint64_t>& handle : handles)
			{
				Result<uint64_t> value = handle.Take();
				REQUIRE(value.has_value());
				sum += *value;
			}
			CHECK(sum == 500500);
		}

		TEST_CASE("JobSystem: move-only values travel into and out of jobs" * doctest::skip(true))
		{
			MainThreadQueue queue;
			JobSystem jobs(1, queue);

			Scope<int> input = CreateScope<int>(20);
			JobHandle<Scope<int>> handle = jobs.Submit([owned = std::move(input)]() mutable -> Result<Scope<int>>
			{
				*owned += 1;
				return std::move(owned);
			});
			Result<Scope<int>> output = handle.Take();
			REQUIRE(output.has_value());
			REQUIRE(*output != nullptr);
			CHECK(**output == 21);
		}

		TEST_CASE("JobSystem: destruction cancels jobs that have not started" * doctest::skip(true))
		{
			MainThreadQueue queue;
			std::latch blockerStarted(1);
			std::latch releaseBlocker(1);
			JobHandle<void> blocker;
			JobHandle<void> queued;
			std::thread releaser;
			{
				JobSystem jobs(1, queue);
				blocker = jobs.Submit([&blockerStarted, &releaseBlocker]() -> Status
				{
					blockerStarted.count_down();
					releaseBlocker.wait();
					return {};
				});
				blockerStarted.wait();
				queued = jobs.Submit([]() -> Status
				{
					return {};
				});

				// The only worker is blocked, so the queued job can complete only through the destructor's cancellation.
				// The releaser frees the worker once that happened, which lets the destructor join it.
				releaser = std::thread([queued, &releaseBlocker]()
				{
					static_cast<void>(queued.Wait());
					releaseBlocker.count_down();
				});
			}
			releaser.join();

			CHECK(blocker.Take().has_value());
			const Status cancelled = queued.Take();
			REQUIRE_FALSE(cancelled.has_value());
			CHECK(cancelled.error().GetCode() == ErrorCode::Cancelled);
		}

		TEST_CASE("JobSystem: the default worker count leaves two hardware threads free" * doctest::skip(true))
		{
			const uint32_t hardware = std::thread::hardware_concurrency();
			const uint32_t expected = hardware > 3 ? hardware - 2 : 1;
			CHECK(JobSystem::GetDefaultWorkerCount() == expected);
		}
	}

}
