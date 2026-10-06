#include "TestsPCH.h"

#include "Engine/Core/EventLog.h"

#include "Support/DeathTest.h"

#include <thread>

namespace Engine {

	static EngineEvent MakeEvent(EngineEventType type, uint64_t id)
	{
		EngineEvent event;
		event.Type = type;
		event.Id = UUID(id);
		return event;
	}

	static std::vector<uint64_t> SeqsOf(const EventReadResult& result)
	{
		std::vector<uint64_t> seqs;
		for (const EngineEvent& event : result.Events)
			seqs.push_back(event.Seq);
		return seqs;
	}

	ENGINE_DEATH_TEST("Core/EventLogAppendOffMainThread")
	{
		EventLog log;
		std::thread other([&log]()
		{
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 1)));
		});
		other.join();
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("EventLog: sequence numbers start at 1 and increase by one")
		{
			EventLog log(16);
			CHECK(log.GetNextSeq() == 1);
			CHECK(log.GetSize() == 0);
			CHECK(log.Append(MakeEvent(EngineEventType::EntityCreated, 10)) == 1);
			CHECK(log.Append(MakeEvent(EngineEventType::ComponentChanged, 10)) == 2);
			CHECK(log.GetSize() == 2);
			CHECK(log.GetCapacity() == 16);
			CHECK(log.GetNextSeq() == 3);

			const EventReadResult result = log.Read(0);
			REQUIRE(result.Events.size() == 2);
			CHECK(result.Events[0].Seq == 1);
			CHECK(result.Events[0].Type == EngineEventType::EntityCreated);
			CHECK(result.Events[1].Seq == 2);
			CHECK(result.NextCursor == 3);
			CHECK(result.DroppedCount == 0);
		}

		TEST_CASE("EventLog: Append overwrites the caller's Seq and keeps every other field")
		{
			EventLog log(4);
			EngineEvent event = MakeEvent(EngineEventType::AssetImportFailed, 42);
			event.Seq = 999;
			event.Tick = 120;
			event.Path = "Assets/Track.glb";
			event.Name = "Track";
			event.Message = "corrupt buffer";
			event.Dirty = true;
			CHECK(log.Append(event) == 1);

			const EventReadResult result = log.Read(0);
			REQUIRE(result.Events.size() == 1);
			const EngineEvent& stored = result.Events[0];
			CHECK(stored.Seq == 1);
			CHECK(stored.Tick == std::optional<uint64_t>(120));
			CHECK(stored.Type == EngineEventType::AssetImportFailed);
			CHECK(stored.Id == UUID(42));
			CHECK(stored.Path == "Assets/Track.glb");
			CHECK(stored.Name == "Track");
			CHECK(stored.Message == "corrupt buffer");
			CHECK(stored.Dirty);
		}

		TEST_CASE("EventLog: an empty log reads nothing and keeps the cursor")
		{
			EventLog log(8);
			const EventReadResult fromStart = log.Read(0);
			CHECK(fromStart.Events.empty());
			CHECK(fromStart.NextCursor == 1);
			CHECK(fromStart.DroppedCount == 0);

			const EventReadResult fromOne = log.Read(1);
			CHECK(fromOne.Events.empty());
			CHECK(fromOne.NextCursor == 1);
			CHECK(fromOne.DroppedCount == 0);
		}

		TEST_CASE("EventLog: a full log drops the oldest events and reports them")
		{
			EventLog log(3);
			for (uint64_t index = 1; index <= 7; ++index)
				static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityDestroyed, index)));

			const EventReadResult result = log.Read(2);
			REQUIRE(result.Events.size() == 3);
			CHECK(result.Events.front().Seq == 5);
			CHECK(result.Events.back().Id == UUID(7));
			CHECK(result.DroppedCount == 3); // events 2 to 4
			CHECK(result.NextCursor == 8);
			CHECK(log.GetSize() == 3);

			// Cursor 0 starts at the oldest held event, so the reader missed nothing it asked for.
			const EventReadResult oldest = log.Read(0);
			CHECK(SeqsOf(oldest) == std::vector<uint64_t>{ 5, 6, 7 });
			CHECK(oldest.DroppedCount == 0);

			// A cursor inside the held range drops nothing.
			const EventReadResult inside = log.Read(6);
			CHECK(SeqsOf(inside) == std::vector<uint64_t>{ 6, 7 });
			CHECK(inside.DroppedCount == 0);
		}

		TEST_CASE("EventLog: the ring keeps the newest events across many wraps")
		{
			constexpr size_t Capacity = 5;
			EventLog log(Capacity);
			for (uint64_t index = 1; index <= 1000; ++index)
				CHECK(log.Append(MakeEvent(EngineEventType::ComponentChanged, index)) == index);

			const EventReadResult result = log.Read(0);
			CHECK(SeqsOf(result) == std::vector<uint64_t>{ 996, 997, 998, 999, 1000 });
			for (const EngineEvent& event : result.Events)
				CHECK(event.Id == UUID(event.Seq));
			CHECK(log.GetSize() == Capacity);
		}

		TEST_CASE("EventLog: a reader that follows NextCursor sees every event exactly once")
		{
			EventLog log(4);
			std::vector<uint64_t> seen;
			uint64_t cursor = 0;
			// Reads pages of two until the reader has caught up; the limit stops each page after its last event.
			const auto catchUp = [&log, &seen, &cursor]()
			{
				for (EventReadResult page = log.Read(cursor, {}, 2); !page.Events.empty(); page = log.Read(cursor, {}, 2))
				{
					CHECK(page.DroppedCount == 0);
					CHECK(page.Events.size() <= 2);
					for (const EngineEvent& event : page.Events)
						seen.push_back(event.Seq);
					cursor = page.NextCursor;
				}
			};

			for (uint64_t index = 1; index <= 50; ++index)
			{
				static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, index)));
				if (index % 3 == 0)
					catchUp();
			}
			catchUp();

			REQUIRE(seen.size() == 50);
			for (size_t index = 0; index < seen.size(); ++index)
				CHECK(seen[index] == index + 1);
			CHECK(cursor == log.GetNextSeq());
		}

		TEST_CASE("EventLog: type filters and limits resume without rescanning")
		{
			EventLog log;
			CHECK(log.GetCapacity() == EventLog::DefaultCapacity);
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 1)));
			static_cast<void>(log.Append(MakeEvent(EngineEventType::AssetReloaded, 2)));
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 3)));
			static_cast<void>(log.Append(MakeEvent(EngineEventType::PlayStateChanged, 0)));

			const std::array<EngineEventType, 1> created = { EngineEventType::EntityCreated };
			const EventReadResult filtered = log.Read(0, created);
			REQUIRE(filtered.Events.size() == 2);
			CHECK(filtered.Events[1].Id == UUID(3));
			CHECK(filtered.NextCursor == 5);

			const EventReadResult limited = log.Read(0, {}, 1);
			REQUIRE(limited.Events.size() == 1);
			CHECK(limited.NextCursor == 2);

			// The limit stops the scan right after the last returned event, so the rest is read next time.
			const std::array<EngineEventType, 2> createdOrPlay = { EngineEventType::EntityCreated, EngineEventType::PlayStateChanged };
			const EventReadResult firstPage = log.Read(0, createdOrPlay, 2);
			CHECK(SeqsOf(firstPage) == std::vector<uint64_t>{ 1, 3 });
			CHECK(firstPage.NextCursor == 4);
			const EventReadResult secondPage = log.Read(firstPage.NextCursor, createdOrPlay, 2);
			CHECK(SeqsOf(secondPage) == std::vector<uint64_t>{ 4 });
			CHECK(secondPage.NextCursor == 5);

			// No matching event: everything was scanned, so the cursor moves past it.
			const std::array<EngineEventType, 1> disconnected = { EngineEventType::AutomationClientDisconnected };
			const EventReadResult none = log.Read(0, disconnected);
			CHECK(none.Events.empty());
			CHECK(none.NextCursor == 5);
		}

		TEST_CASE("EventLog: a zero limit returns nothing and keeps the cursor")
		{
			EventLog log(8);
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 1)));
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 2)));

			const EventReadResult result = log.Read(2, {}, 0);
			CHECK(result.Events.empty());
			CHECK(result.NextCursor == 2);
		}

		TEST_CASE("EventLog: a cursor past the next sequence number restarts at the next event")
		{
			EventLog log(8);
			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityCreated, 1)));

			// For example a cursor kept from a previous editor session.
			const EventReadResult stale = log.Read(500);
			CHECK(stale.Events.empty());
			CHECK(stale.DroppedCount == 0);
			CHECK(stale.NextCursor == 2);

			static_cast<void>(log.Append(MakeEvent(EngineEventType::EntityDestroyed, 1)));
			const EventReadResult next = log.Read(stale.NextCursor);
			CHECK(SeqsOf(next) == std::vector<uint64_t>{ 2 });
		}

		TEST_CASE("EventLog: type names round-trip case-insensitively")
		{
			const std::array types = {
				EngineEventType::EntityCreated,
				EngineEventType::EntityDestroyed,
				EngineEventType::ComponentChanged,
				EngineEventType::SceneOpened,
				EngineEventType::SceneChangedOnDisk,
				EngineEventType::PlayStateChanged,
				EngineEventType::AssetReloaded,
				EngineEventType::AssetImportFailed,
				EngineEventType::ScriptErrorRaised,
				EngineEventType::DiagnosticsChanged,
				EngineEventType::AutomationClientDisconnected,
			};
			for (const EngineEventType type : types)
				CHECK(EngineEventTypeFromString(EngineEventTypeToString(type)) == type);

			CHECK(EngineEventTypeToString(EngineEventType::SceneChangedOnDisk) == "SceneChangedOnDisk");
			CHECK(EngineEventTypeFromString("scenechangedondisk") == EngineEventType::SceneChangedOnDisk);
			CHECK(EngineEventTypeFromString("ENTITYCREATED") == EngineEventType::EntityCreated);
			CHECK_FALSE(EngineEventTypeFromString("EntityMoved").has_value());
			CHECK_FALSE(EngineEventTypeFromString("").has_value());
			CHECK_FALSE(EngineEventTypeFromString("EntityCreated ").has_value());
		}

		TEST_CASE("EventLog: Append off the main thread is a programmer error")
		{
			ENGINE_CHECK_DEATH("Core/EventLogAppendOffMainThread", "EventLog::Append called off the main thread");
		}
	}

}
