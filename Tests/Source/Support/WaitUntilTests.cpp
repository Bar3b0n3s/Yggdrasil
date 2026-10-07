#include "TestsPCH.h"

#include "Support/WaitUntil.h"

#include <atomic>
#include <thread>

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("WaitUntil: returns once another thread makes the condition true")
		{
			std::atomic<bool> done = false;
			std::thread worker([&done]()
			{
				done = true;
			});
			CHECK(Test::WaitUntil([&done]()
			{
				return done.load();
			}));
			worker.join();
		}

		TEST_CASE("WaitUntil: a condition that holds at once is polled once")
		{
			int polls = 0;
			CHECK(Test::WaitUntil([&polls]()
			{
				++polls;
				return true;
			}));
			CHECK(polls == 1);
		}

		TEST_CASE("WaitUntil: a condition that never holds fails after the bound")
		{
			int polls = 0;
			CHECK_FALSE(Test::WaitUntil([&polls]()
			{
				++polls;
				return false;
			}, std::chrono::steady_clock::duration::zero()));
			CHECK(polls >= 1);
		}
	}

}
