#include "TestsPCH.h"

#include "Engine/Core/Jobs/JobSystem.h"

#include "Support/DeathTest.h"

#include <atomic>
#include <latch>
#include <thread>

namespace Engine {

	ENGINE_DEATH_TEST("Core/JobThrows")
	{
		MainThreadQueue queue;
		JobSystem jobs(1, queue);
		JobHandle<int> handle = jobs.Submit([]() -> Result<int>
		{
			// std::stoi throws std::invalid_argument: a standard-library exception escaping a job. It parses in the C
			// runtime, so the optimizer cannot prove the throw and flag the job's completion as unreachable (C4702).
			return std::stoi(std::string("not a number"));
		});
		static_cast<void>(handle.Wait());
	}

	ENGINE_DEATH_TEST("Core/JobWaitsForIdle")
	{
		MainThreadQueue queue;
		JobSystem jobs(0, queue);
		static_cast<void>(jobs.Submit([&jobs]() -> Status
		{
			jobs.WaitIdle();
			return {};
		}));
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("JobSystem: inline mode preserves submission order")
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

		TEST_CASE("JobSystem: Wait returns the result and Take moves it out")
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
			CHECK(handle.IsReady());

			const Result<std::string> taken = handle.Take();
			REQUIRE(taken.has_value());
			CHECK(*taken == "cooked");

			const JobHandle<int> invalid;
			CHECK_FALSE(invalid.IsValid());
		}

		TEST_CASE("JobSystem: errors come back as results")
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

		TEST_CASE("JobSystem: continuations run on the main thread at the next Drain")
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

		TEST_CASE("JobSystem: ContinueOnMainThread posts at once for a job that already completed")
		{
			MainThreadQueue queue;
			JobSystem jobs(1, queue);

			JobHandle<int> handle = jobs.Submit([]() -> Result<int>
			{
				return 11;
			});
			REQUIRE(handle.Wait().has_value());

			int received = 0;
			jobs.ContinueOnMainThread(handle, [&received](Result<int> result)
			{
				REQUIRE(result.has_value());
				received = *result;
			});
			CHECK(queue.GetPendingCount() == 1);
			CHECK(queue.Drain() == 1);
			CHECK(received == 11);
		}

		TEST_CASE("JobSystem: worker mode completes every job")
		{
			MainThreadQueue queue;
			JobSystem jobs(4, queue);
			CHECK(jobs.GetWorkerCount() == 4);
			CHECK_FALSE(jobs.IsInline());

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

		TEST_CASE("JobSystem: a single worker starts jobs in submission order")
		{
			MainThreadQueue queue;
			JobSystem jobs(1, queue);

			// Only the one worker writes `started`; WaitIdle synchronizes with it through the job system's mutex.
			std::vector<int> started;
			for (int index = 0; index < 100; ++index)
			{
				static_cast<void>(jobs.Submit([&started, index]() -> Status
				{
					started.push_back(index);
					return {};
				}));
			}
			jobs.WaitIdle();

			REQUIRE(started.size() == 100);
			for (int index = 0; index < 100; ++index)
				CHECK(started[static_cast<size_t>(index)] == index);
		}

		TEST_CASE("JobSystem: WaitIdle returns only after every submitted job completed")
		{
			constexpr int WorkerCount = 4;
			constexpr int QueuedCount = 200;
			MainThreadQueue queue;
			JobSystem jobs(WorkerCount, queue);

			// The first jobs occupy every worker until the gate opens, so the rest queue up behind them. The gate opens on
			// another thread at an arbitrary moment; whenever that is, WaitIdle must not return before all of them ran.
			std::latch gate(1);
			std::atomic<int> completed = 0;
			std::vector<JobHandle<void>> handles;
			for (int index = 0; index < WorkerCount; ++index)
			{
				handles.push_back(jobs.Submit([&gate, &completed]() -> Status
				{
					gate.wait();
					++completed;
					return {};
				}));
			}
			for (int index = 0; index < QueuedCount; ++index)
			{
				handles.push_back(jobs.Submit([&completed]() -> Status
				{
					++completed;
					return {};
				}));
			}

			std::thread opener([&gate]()
			{
				gate.count_down();
			});
			jobs.WaitIdle();
			opener.join();

			CHECK(completed == WorkerCount + QueuedCount);
			for (const JobHandle<void>& handle : handles)
				CHECK(handle.IsReady());
		}

		TEST_CASE("JobSystem: jobs submit further jobs and WaitIdle waits for those too")
		{
			constexpr int ParentCount = 8;
			constexpr int ChildrenPerParent = 10;
			uint32_t workerCount = 0;
			SUBCASE("inline")
			{
				workerCount = 0;
			}
			SUBCASE("workers")
			{
				workerCount = 3;
			}

			MainThreadQueue queue;
			JobSystem jobs(workerCount, queue);
			std::atomic<int> children = 0;
			for (int parent = 0; parent < ParentCount; ++parent)
			{
				static_cast<void>(jobs.Submit([&jobs, &children]() -> Status
				{
					for (int child = 0; child < ChildrenPerParent; ++child)
					{
						static_cast<void>(jobs.Submit([&children]() -> Status
						{
							++children;
							return {};
						}));
					}
					return {};
				}));
			}

			// A parent is still running when it queues its children, so the system cannot look idle in between.
			jobs.WaitIdle();
			CHECK(children == ParentCount * ChildrenPerParent);
		}

		TEST_CASE("JobSystem: concurrent submitters lose no job and no continuation")
		{
			constexpr int SubmitterCount = 4;
			constexpr int JobsPerSubmitter = 250;
			MainThreadQueue queue;
			JobSystem jobs(3, queue);

			// Continuations run only on the main thread (Drain), so `received` needs no lock.
			std::vector<int> received;
			std::vector<std::thread> submitters;
			for (int submitter = 0; submitter < SubmitterCount; ++submitter)
			{
				submitters.emplace_back([&jobs, &received, submitter]()
				{
					for (int index = 0; index < JobsPerSubmitter; ++index)
					{
						const int value = submitter * JobsPerSubmitter + index;
						JobHandle<int> handle = jobs.Submit([value]() -> Result<int>
						{
							return value;
						});
						jobs.ContinueOnMainThread(handle, [&received](Result<int> result)
						{
							REQUIRE(result.has_value());
							received.push_back(*result);
						});
					}
				});
			}
			for (std::thread& submitter : submitters)
				submitter.join();

			jobs.WaitIdle();
			CHECK(queue.Drain() == static_cast<uint32_t>(SubmitterCount * JobsPerSubmitter));
			std::ranges::sort(received);
			REQUIRE(received.size() == static_cast<size_t>(SubmitterCount * JobsPerSubmitter));
			for (size_t index = 0; index < received.size(); ++index)
				CHECK(received[index] == static_cast<int>(index));
		}

		TEST_CASE("JobSystem: move-only values travel into and out of jobs")
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

		TEST_CASE("JobSystem: a job's captures are released by the time WaitIdle returns")
		{
			uint32_t workerCount = 0;
			SUBCASE("inline")
			{
				workerCount = 0;
			}
			SUBCASE("workers")
			{
				workerCount = 2;
			}

			MainThreadQueue queue;
			JobSystem jobs(workerCount, queue);
			Ref<int> witness = CreateRef<int>(5);
			JobHandle<int> handle = jobs.Submit([witness]() -> Result<int>
			{
				return *witness;
			});
			jobs.WaitIdle();

			CHECK(witness.use_count() == 1); // the handle keeps the result, not the job
			REQUIRE(handle.Wait().has_value());
			CHECK(*handle.Wait() == 5);
		}

		TEST_CASE("JobSystem: destruction cancels jobs that have not started")
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

		TEST_CASE("JobSystem: continuations of cancelled jobs are still posted, in submission order")
		{
			MainThreadQueue queue;
			std::latch blockerStarted(1);
			std::latch releaseBlocker(1);
			std::vector<std::string> outcomes;
			std::thread releaser;
			{
				JobSystem jobs(1, queue);
				static_cast<void>(jobs.Submit([&blockerStarted, &releaseBlocker]() -> Status
				{
					blockerStarted.count_down();
					releaseBlocker.wait();
					return {};
				}));
				blockerStarted.wait();

				// `signal` has no continuation, so the releaser may wait on it: the destructor cancels the queue in
				// submission order, and cancels every queued job before it joins the worker.
				JobHandle<void> signal = jobs.Submit([]() -> Status
				{
					return {};
				});
				for (const char* name : { "first", "second" })
				{
					JobHandle<void> handle = jobs.Submit([]() -> Status
					{
						return {};
					});
					jobs.ContinueOnMainThread(handle, [&outcomes, name](Status status)
					{
						const bool isCancelled = !status.has_value() && status.error().GetCode() == ErrorCode::Cancelled;
						outcomes.push_back(std::format("{}:{}", name, isCancelled ? "cancelled" : "ran"));
					});
				}
				CHECK(queue.GetPendingCount() == 0);

				releaser = std::thread([signal, &releaseBlocker]()
				{
					static_cast<void>(signal.Wait());
					releaseBlocker.count_down();
				});
			}
			releaser.join();

			CHECK(queue.Drain() == 2);
			CHECK(outcomes == std::vector<std::string>{ "first:cancelled", "second:cancelled" });
		}

		TEST_CASE("JobSystem: continuations run at Drain in completion order, so a generation check can drop a stale result")
		{
			// What JobSystem provides for Architecture §7.5 rule 2 ("newest wins"): continuations run on the main thread at
			// Drain, in completion order, with the posting code's captures. The generation check that drops a stale result
			// is the caller's code (here, the test's lambda); JobSystem has no notion of staleness. The rule itself is
			// tested with M6's AssetManager, not here.
			MainThreadQueue queue;
			uint64_t newestGeneration = 0;
			std::vector<std::string> applied;
			std::vector<uint64_t> dropped;
			const auto continueWithGeneration = [&newestGeneration, &applied, &dropped](uint64_t generation)
			{
				return [&newestGeneration, &applied, &dropped, generation](Result<std::string> result)
				{
					if (generation < newestGeneration)
					{
						dropped.push_back(generation);
						return;
					}
					REQUIRE(result.has_value());
					applied.push_back(*result);
				};
			};

			SUBCASE("a newer request supersedes an older one that completed first")
			{
				JobSystem jobs(0, queue);
				newestGeneration = 1;
				JobHandle<std::string> older = jobs.Submit([]() -> Result<std::string>
				{
					return std::string("old texture");
				});
				jobs.ContinueOnMainThread(older, continueWithGeneration(1));
				newestGeneration = 2;
				JobHandle<std::string> newer = jobs.Submit([]() -> Result<std::string>
				{
					return std::string("new texture");
				});
				jobs.ContinueOnMainThread(newer, continueWithGeneration(2));

				CHECK(queue.Drain() == 2);
			}

			SUBCASE("a slow older job completes after the newer one")
			{
				JobSystem jobs(2, queue);
				std::latch releaseOlder(1);
				newestGeneration = 1;
				JobHandle<std::string> older = jobs.Submit([&releaseOlder]() -> Result<std::string>
				{
					releaseOlder.wait();
					return std::string("old texture");
				});
				jobs.ContinueOnMainThread(older, continueWithGeneration(1));
				newestGeneration = 2;
				JobHandle<std::string> newer = jobs.Submit([]() -> Result<std::string>
				{
					return std::string("new texture");
				});

				// The newer job has completed before its continuation is attached, so that continuation is posted first;
				// only then may the older job finish.
				REQUIRE(newer.Wait().has_value());
				jobs.ContinueOnMainThread(newer, continueWithGeneration(2));
				releaseOlder.count_down();
				jobs.WaitIdle();

				CHECK(queue.Drain() == 2);
			}

			CHECK(applied == std::vector<std::string>{ "new texture" });
			CHECK(dropped == std::vector<uint64_t>{ 1 });
		}

		TEST_CASE("JobSystem: the default worker count leaves two hardware threads free")
		{
			const uint32_t hardware = std::thread::hardware_concurrency();
			const uint32_t expected = hardware > 3 ? hardware - 2 : 1;
			CHECK(JobSystem::GetDefaultWorkerCount() == expected);
		}

		TEST_CASE("JobSystem: a job that throws ends the process with exit code 4")
		{
			ENGINE_CHECK_DEATH("Core/JobThrows", "Fatal error (UnhandledException): A job threw an exception");
		}

		TEST_CASE("JobSystem: WaitIdle from inside one of its jobs is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/JobWaitsForIdle", "WaitIdle called from inside one of its own jobs");
		}
	}

}
