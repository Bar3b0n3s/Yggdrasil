#include "TestsPCH.h"

#include "Engine/Core/Jobs/MainThreadQueue.h"

#include "Support/DeathTest.h"

#include <atomic>
#include <thread>

namespace Engine {

	namespace {

		// A task that posts a copy of itself every time it runs.
		struct Repost
		{
			void operator()() const
			{
				++*Runs;
				Queue->Post(Repost{ Queue, Runs });
			}

			MainThreadQueue* Queue = nullptr;
			int* Runs = nullptr;
		};

	}

	ENGINE_DEATH_TEST("Core/MainThreadQueueDrainOffMainThread")
	{
		MainThreadQueue queue;
		std::thread other([&queue]()
		{
			static_cast<void>(queue.Drain());
		});
		other.join();
	}

	ENGINE_DEATH_TEST("Core/MainThreadQueueDrainInsideTask")
	{
		MainThreadQueue queue;
		queue.Post([&queue]()
		{
			static_cast<void>(queue.Drain());
		});
		static_cast<void>(queue.Drain());
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("MainThreadQueue: completions drain in order")
		{
			MainThreadQueue queue;
			std::vector<int> order;
			for (int index = 0; index < 5; ++index)
			{
				queue.Post([&order, index]()
				{
					order.push_back(index);
				});
			}
			CHECK(queue.GetPendingCount() == 5);
			CHECK(order.empty());

			CHECK(queue.Drain() == 5);
			CHECK(order == std::vector<int>{ 0, 1, 2, 3, 4 });
			CHECK(queue.GetPendingCount() == 0);
			CHECK(queue.Drain() == 0);
		}

		TEST_CASE("MainThreadQueue: tasks posted while draining run at the next Drain")
		{
			MainThreadQueue queue;
			std::vector<std::string> order;
			queue.Post([&queue, &order]()
			{
				order.push_back("first");
				queue.Post([&order]()
				{
					order.push_back("reposted");
				});
			});
			queue.Post([&order]()
			{
				order.push_back("second");
			});

			CHECK(queue.Drain() == 2);
			CHECK(order == std::vector<std::string>{ "first", "second" });
			CHECK(queue.GetPendingCount() == 1);
			CHECK(queue.Drain() == 1);
			CHECK(order.back() == "reposted");
		}

		TEST_CASE("MainThreadQueue: a task that re-posts itself runs once per Drain")
		{
			// A self-perpetuating task (a file-watcher poll that schedules its next round) never starves the frame.
			MainThreadQueue queue;
			int runs = 0;
			queue.Post(Repost{ &queue, &runs });

			for (int frame = 1; frame <= 3; ++frame)
			{
				CHECK(queue.Drain() == 1);
				CHECK(runs == frame);
			}
			CHECK(queue.GetPendingCount() == 1);
		}

		TEST_CASE("MainThreadQueue: each task is released right after it runs")
		{
			MainThreadQueue queue;
			Ref<int> first = CreateRef<int>(1);
			Ref<int> second = CreateRef<int>(2);
			std::vector<long> useCounts;
			queue.Post([first, &second, &useCounts]()
			{
				useCounts.push_back(first.use_count());  // this task's copy and the local
				useCounts.push_back(second.use_count()); // the local and the second task's copy
			});
			queue.Post([second, &first, &useCounts]()
			{
				// The first task, and its copy of `first`, are gone before the second one runs.
				useCounts.push_back(first.use_count());
				useCounts.push_back(second.use_count());
			});

			CHECK(queue.Drain() == 2);
			CHECK(useCounts == std::vector<long>{ 2, 2, 1, 2 });
			CHECK(first.use_count() == 1);
			CHECK(second.use_count() == 1);
		}

		TEST_CASE("MainThreadQueue: posting from several threads loses nothing and keeps each thread's order")
		{
			constexpr int ThreadCount = 4;
			constexpr int TasksPerThread = 250;
			MainThreadQueue queue;
			std::vector<std::pair<int, int>> received;

			std::vector<std::thread> threads;
			for (int thread = 0; thread < ThreadCount; ++thread)
			{
				threads.emplace_back([&queue, &received, thread]()
				{
					for (int index = 0; index < TasksPerThread; ++index)
					{
						queue.Post([&received, thread, index]()
						{
							received.emplace_back(thread, index);
						});
					}
				});
			}
			for (std::thread& thread : threads)
				thread.join();

			CHECK(queue.Drain() == static_cast<uint32_t>(ThreadCount * TasksPerThread));
			REQUIRE(received.size() == static_cast<size_t>(ThreadCount * TasksPerThread));
			std::array<int, ThreadCount> next{};
			for (const auto& [thread, index] : received)
			{
				CHECK(index == next[static_cast<size_t>(thread)]);
				++next[static_cast<size_t>(thread)];
			}
		}

		TEST_CASE("MainThreadQueue: draining while other threads post loses nothing and keeps each thread's order")
		{
			constexpr int ThreadCount = 4;
			constexpr int TasksPerThread = 500;
			MainThreadQueue queue;
			// Tasks run only on this thread (Drain), so `received` needs no lock.
			std::vector<std::pair<int, int>> received;
			std::atomic<int> finishedThreads = 0;

			std::vector<std::thread> threads;
			for (int thread = 0; thread < ThreadCount; ++thread)
			{
				threads.emplace_back([&queue, &received, &finishedThreads, thread]()
				{
					for (int index = 0; index < TasksPerThread; ++index)
					{
						queue.Post([&received, thread, index]()
						{
							received.emplace_back(thread, index);
						});
					}
					++finishedThreads;
				});
			}

			// Every Drain races the posters; the loop ends once all of them are done, then one last Drain takes the rest.
			uint64_t drained = 0;
			while (finishedThreads.load() < ThreadCount)
				drained += queue.Drain();
			for (std::thread& thread : threads)
				thread.join();
			drained += queue.Drain();

			CHECK(drained == static_cast<uint64_t>(ThreadCount * TasksPerThread));
			REQUIRE(received.size() == static_cast<size_t>(ThreadCount * TasksPerThread));
			std::array<int, ThreadCount> next{};
			for (const auto& [thread, index] : received)
			{
				CHECK(index == next[static_cast<size_t>(thread)]);
				++next[static_cast<size_t>(thread)];
			}
			CHECK(queue.GetPendingCount() == 0);
		}

		TEST_CASE("MainThreadQueue: pending tasks are destroyed without running")
		{
			bool ran = false;
			Ref<int> witness = CreateRef<int>(0);
			{
				MainThreadQueue queue;
				queue.Post([&ran, witness]()
				{
					ran = true;
				});
				CHECK(witness.use_count() == 2);
			}
			CHECK_FALSE(ran);
			CHECK(witness.use_count() == 1);
		}

		TEST_CASE("MainThreadQueue: IsMainThread is true only on the constructing thread")
		{
			MainThreadQueue queue;
			CHECK(queue.IsMainThread());

			bool otherIsMain = true;
			std::thread other([&queue, &otherIsMain]()
			{
				otherIsMain = queue.IsMainThread();
			});
			other.join();
			CHECK_FALSE(otherIsMain);
		}

		TEST_CASE("MainThreadQueue: Drain off the main thread is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/MainThreadQueueDrainOffMainThread", "MainThreadQueue::Drain called off the main thread");
		}

		TEST_CASE("MainThreadQueue: Drain from inside a task is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/MainThreadQueueDrainInsideTask", "MainThreadQueue::Drain called from inside a task");
		}
	}

}
