#include "TestsPCH.h"

#include "Engine/Core/Jobs/MainThreadQueue.h"

#include <thread>

namespace Engine {

	TEST_SUITE("Core")
	{
		TEST_CASE("MainThreadQueue: completions drain in order" * doctest::skip(true))
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

		TEST_CASE("MainThreadQueue: tasks posted while draining run at the next Drain" * doctest::skip(true))
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
			CHECK(queue.Drain() == 1);
			CHECK(order.back() == "reposted");
		}

		TEST_CASE("MainThreadQueue: posting from several threads loses nothing and keeps each thread's order" * doctest::skip(true))
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

		TEST_CASE("MainThreadQueue: pending tasks are destroyed without running" * doctest::skip(true))
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

		TEST_CASE("MainThreadQueue: IsMainThread is true only on the constructing thread" * doctest::skip(true))
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
	}

}
