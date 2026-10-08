#include "TestsPCH.h"

#include "Engine/Physics/ContactBuffer.h"

#include <thread>

// The contact buffer (Architecture §9.4: worker threads append, the main thread drains).

namespace Engine {

	TEST_SUITE("Physics")
	{
		TEST_CASE("ContactBuffer: appends from many threads and drains every record exactly once")
		{
			ContactBuffer buffer;
			constexpr uint32_t ThreadCount = 4;
			constexpr uint32_t RecordsPerThread = 1000;
			{
				std::vector<std::thread> threads;
				for (uint32_t thread = 0; thread < ThreadCount; ++thread)
				{
					threads.emplace_back([&buffer, thread]()
					{
						for (uint32_t record = 0; record < RecordsPerThread; ++record)
							buffer.Append(ContactEvent{ .BodyA = BodyHandle(thread), .BodyB = BodyHandle(record) });
					});
				}
				for (std::thread& thread : threads)
					thread.join();
			}
			CHECK(buffer.GetSize() == ThreadCount * RecordsPerThread);
			const std::vector<ContactEvent> drained = buffer.Drain();
			CHECK(drained.size() == ThreadCount * RecordsPerThread);
			CHECK(buffer.GetSize() == 0);
			CHECK(buffer.Drain().empty());
			// Every (thread, record) pair arrived once, whatever the interleaving.
			std::vector<uint32_t> perThread(ThreadCount, 0);
			for (const ContactEvent& event : drained)
				++perThread.at(event.BodyA.GetValue());
			for (const uint32_t count : perThread)
				CHECK(count == RecordsPerThread);
		}

		TEST_CASE("ContactBuffer: Clear discards the records and keeps the buffer usable")
		{
			ContactBuffer buffer;
			buffer.Append(ContactEvent{ .Kind = ContactEventKind::Removed });
			buffer.Clear();
			CHECK(buffer.GetSize() == 0);
			const ContactEvent added{ .Kind = ContactEventKind::Added, .ColliderA = 3, .IsSensor = true };
			buffer.Append(added);
			const std::vector<ContactEvent> drained = buffer.Drain();
			REQUIRE(drained.size() == 1);
			CHECK(drained.front() == added);
		}
	}

}
