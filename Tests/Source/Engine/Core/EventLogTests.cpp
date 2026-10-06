#include "TestsPCH.h"

#include "Engine/Core/EventLog.h"

namespace Engine {

	static EngineEvent MakeEvent(EngineEventType type, uint64_t id)
	{
		EngineEvent event;
		event.Type = type;
		event.Id = UUID(id);
		return event;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("EventLog: sequence numbers start at 1 and increase by one" * doctest::skip(true))
		{
			EventLog log(16);
			CHECK(log.GetNextSeq() == 1);
			CHECK(log.Append(MakeEvent(EngineEventType::EntityCreated, 10)) == 1);
			CHECK(log.Append(MakeEvent(EngineEventType::ComponentChanged, 10)) == 2);
			CHECK(log.GetSize() == 2);
			CHECK(log.GetCapacity() == 16);

			const EventReadResult result = log.Read(0);
			REQUIRE(result.Events.size() == 2);
			CHECK(result.Events[0].Seq == 1);
			CHECK(result.Events[0].Type == EngineEventType::EntityCreated);
			CHECK(result.Events[1].Seq == 2);
			CHECK(result.NextCursor == 3);
			CHECK(result.DroppedCount == 0);
		}

		TEST_CASE("EventLog: a full log drops the oldest events and reports them" * doctest::skip(true))
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
		}

		TEST_CASE("EventLog: type filters and limits resume without rescanning" * doctest::skip(true))
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
		}

		TEST_CASE("EventLog: type names round-trip case-insensitively" * doctest::skip(true))
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
			CHECK_FALSE(EngineEventTypeFromString("EntityMoved").has_value());
		}
	}

}
