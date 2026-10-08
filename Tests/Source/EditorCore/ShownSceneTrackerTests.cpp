#include "TestsPCH.h"

#include "EditorCore/ShownSceneTracker.h"

#include "Engine/Core/EventLog.h"
#include "Engine/Scene/Scene.h"
#include "Support/SceneTestFixture.h"

// The editor's notice of a changed shown scene for the stale-mirror collections (EditorCore/ShownSceneTracker.h;
// Docs/Decisions/0013-m8-decisions.md decisions 7 and 27): a scene opened, closed or swapped, play mode started or stopped,
// a scene replaced at the same address (seen through its event only) and dropped events.

namespace Engine {

	namespace {

		EngineEvent MakeEvent(EngineEventType type)
		{
			EngineEvent event;
			event.Type = type;
			return event;
		}

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ShownSceneTracker: opening, swapping and closing the edit scene are changes")
		{
			Test::SceneTestFixture fixture;
			const Scope<Scene> other = fixture.CreateEmptyScene();
			const EventLog events;
			ShownSceneTracker tracker;
			// Nothing shown and nothing logged: no change.
			CHECK_FALSE(tracker.Update(nullptr, nullptr, events));
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events)); // opened
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
			CHECK(tracker.Update(other.get(), nullptr, events)); // swapped
			CHECK_FALSE(tracker.Update(other.get(), nullptr, events));
			CHECK(tracker.Update(nullptr, nullptr, events)); // closed
			CHECK_FALSE(tracker.Update(nullptr, nullptr, events));
		}

		TEST_CASE("ShownSceneTracker: starting and stopping play mode are changes")
		{
			Test::SceneTestFixture fixture;
			const Scope<Scene> play = fixture.CreateEmptyScene(true);
			const EventLog events;
			ShownSceneTracker tracker;
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			CHECK(tracker.Update(&fixture.GetScene(), play.get(), events)); // play started
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), play.get(), events));
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events)); // play stopped
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
		}

		TEST_CASE("ShownSceneTracker: SceneOpened and PlayStateChanged events are changes at the same addresses")
		{
			// A scene or session replaced between two calls by one at the same address shows only through its event; other
			// events are not changes.
			Test::SceneTestFixture fixture;
			EventLog events;
			ShownSceneTracker tracker;
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			events.Append(MakeEvent(EngineEventType::EntityCreated));
			events.Append(MakeEvent(EngineEventType::ComponentChanged));
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
			events.Append(MakeEvent(EngineEventType::SceneOpened));
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events)); // each event counts once
			events.Append(MakeEvent(EngineEventType::PlayStateChanged));
			events.Append(MakeEvent(EngineEventType::EntityDestroyed));
			events.Append(MakeEvent(EngineEventType::PlayStateChanged));
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
		}

		TEST_CASE("ShownSceneTracker: events the log dropped before they were read count as a change")
		{
			// The log keeps two events: three unrelated ones since the last call overwrite one the tracker never read, which
			// might have been a SceneOpened.
			Test::SceneTestFixture fixture;
			EventLog events(2);
			ShownSceneTracker tracker;
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			events.Append(MakeEvent(EngineEventType::EntityCreated));
			events.Append(MakeEvent(EngineEventType::EntityCreated));
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
			for (int index = 0; index < 3; ++index)
				events.Append(MakeEvent(EngineEventType::EntityCreated));
			CHECK(tracker.Update(&fixture.GetScene(), nullptr, events));
			CHECK_FALSE(tracker.Update(&fixture.GetScene(), nullptr, events));
		}

		TEST_CASE("ShownSceneTracker: the first call reads the events logged before it")
		{
			Test::SceneTestFixture fixture;
			EventLog events;
			events.Append(MakeEvent(EngineEventType::SceneOpened));
			ShownSceneTracker tracker;
			CHECK(tracker.Update(nullptr, nullptr, events));
			CHECK_FALSE(tracker.Update(nullptr, nullptr, events));
		}
	}

}
