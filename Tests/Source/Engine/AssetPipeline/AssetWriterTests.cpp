#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetWriter.h"

#include "Engine/Platform/PollingFileWatcher.h"
#include "Support/AssetTestFixture.h"

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AssetWriter: writes are marked known and reported to the listener" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), { .Root = fixture.ProjectPath("Assets"), .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start().has_value());
			AssetWriter writer(fixture.GetVfs());
			writer.SetWatcher(&watcher);
			std::vector<AssetWriteEvent> events;
			writer.SetListener([&events](const AssetWriteEvent& event)
			{
				events.push_back(event);
			});

			const std::string text = "{}";
			REQUIRE(writer.Write(fixture.ProjectPath("Assets/Red.material"), AsBytes(text)).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Red.material"), fixture.ProjectPath("Assets/Materials/Red.material")).has_value());
			REQUIRE(writer.CreateDirectories(fixture.ProjectPath("Assets/Empty")).has_value());
			REQUIRE(writer.Remove(fixture.ProjectPath("Assets/Materials/Red.material")).has_value());
			REQUIRE(events.size() == 4);
			CHECK(events[0].Kind == AssetWriteKind::Written);
			CHECK(events[1].Kind == AssetWriteKind::Moved);
			CHECK(events[1].From == fixture.ProjectPath("Assets/Red.material"));
			CHECK(events[1].Path == fixture.ProjectPath("Assets/Materials/Red.material"));
			CHECK(events[2].Kind == AssetWriteKind::DirectoryCreated);
			CHECK(events[3].Kind == AssetWriteKind::Removed);

			// None of the writer's changes comes back as an external change (§7.5 race rule 1).
			Result<std::vector<FileChange>> changes = watcher.Poll(10.0);
			REQUIRE(changes.has_value());
			CHECK(changes->empty());
		}

		TEST_CASE("AssetWriter: a failed write reports nothing and dry runs never reach the watcher" * doctest::skip(true))
		{
			Test::AssetTestFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), { .Root = fixture.ProjectPath("Assets"), .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start().has_value());
			AssetWriter writer(fixture.GetVfs());
			writer.SetWatcher(&watcher);
			size_t reported = 0;
			writer.SetListener([&reported](const AssetWriteEvent&)
			{
				++reported;
			});
			const std::string text = "x";
			CHECK_FALSE(writer.Write(fixture.ProjectPath("Assets/Missing/Dir/File.txt"), AsBytes(text)).has_value());
			CHECK(reported == 0);

			writer.SetDryRun(true);
			CHECK(writer.IsDryRun());
			REQUIRE(writer.Write(fixture.ProjectPath("Assets/DryRun.material"), AsBytes(text)).has_value());
			CHECK(reported == 1);
			// The watcher was not told: the write appears as an external change (here, the overlay is the disk).
			Result<std::vector<FileChange>> first = watcher.Poll(1.0);
			REQUIRE(first.has_value());
			Result<std::vector<FileChange>> changes = watcher.Poll(2.0);
			REQUIRE(changes.has_value());
			REQUIRE(changes->size() == 1);
			CHECK(changes->front().Path == fixture.ProjectPath("Assets/DryRun.material"));
		}
	}

}
