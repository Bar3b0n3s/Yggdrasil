#include "TestsPCH.h"

#include "Engine/AssetPipeline/AssetWriter.h"

#include "Engine/Core/Hash.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/PollingFileWatcher.h"
#include "Support/AssetTestFixture.h"
#include "Support/TempDirectory.h"

#include <algorithm>

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("AssetWriter: the mutation guard precedes backups parents watcher updates and notifications")
		{
			const Test::TempDirectory directory("AssetWriterGuard");
			auto mount = NativeDirectoryMount::Create(directory.GetPath());
			REQUIRE(mount);
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", std::move(*mount)));
			const VfsPath assets = Test::ParseVfsPath("project://Assets");
			const VfsPath source = Test::ParseVfsPath("project://Assets/Source.txt");
			const VfsPath backup = Test::ParseVfsPath("project://Assets/Source.txt.bak");
			const VfsPath fresh = Test::ParseVfsPath("project://Assets/Fresh.txt");
			const VfsPath parent = Test::ParseVfsPath("project://Assets/NewParent");
			const VfsPath moved = Test::ParseVfsPath("project://Assets/NewParent/Moved.txt");
			const VfsPath folder = Test::ParseVfsPath("project://Assets/Another/Child");
			REQUIRE(vfs.CreateDirectories(assets));
			REQUIRE(vfs.WriteFileAtomic(source, AsBytes("older")));
			REQUIRE(vfs.WriteFileAtomic(source, AsBytes("original")));
			REQUIRE(vfs.ReadText(backup) == "older");
			PollingFileWatcher watcher(vfs, { .Root = assets, .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start());
			AssetWriter writer(vfs);
			writer.SetWatcher(&watcher);
			uint32_t admitted = 0;
			uint32_t reported = 0;
			bool allowed = false;
			writer.SetMutationGuard([&admitted, &allowed]() -> Status
			{
				++admitted;
				return allowed ? Status{} : MakeError(ErrorCode::PermissionDenied, "test writer policy");
			});
			writer.SetListener([&reported](const AssetWriteEvent&)
			{
				++reported;
			});
			const Status denied[] = {
				writer.Write(source, AsBytes("denied")), writer.Write(fresh, AsBytes("denied")),
				writer.Remove(source), writer.Move(source, moved), writer.CreateDirectories(folder)
			};
			for (const Status& result : denied)
			{
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::PermissionDenied);
				CHECK(result.error().GetMessageText() == "test writer policy");
			}
			writer.SetDryRun(true);
			CHECK_FALSE(writer.Write(source, AsBytes("still denied")));
			writer.SetDryRun(false);
			CHECK(admitted == 6);
			CHECK(reported == 0);
			CHECK(vfs.ReadText(source) == "original");
			CHECK(vfs.ReadText(backup) == "older");
			CHECK_FALSE(vfs.Exists(fresh));
			CHECK_FALSE(vfs.Exists(parent));
			CHECK_FALSE(vfs.Exists(folder.GetParent()));
			// A denied write must not mark an external change known and hide it from the watcher.
			REQUIRE(vfs.WriteFileAtomic(source, AsBytes("external bytes")));
			CHECK_FALSE(writer.Write(source, AsBytes("denied")));
			REQUIRE(watcher.Poll(1.0));
			const auto external = watcher.Poll(2.0);
			REQUIRE(external);
			const auto sourceChange = std::ranges::find(*external, source, &FileChange::Path);
			REQUIRE(sourceChange != external->end());
			CHECK(sourceChange->Kind == FileChangeKind::Modified);
			allowed = true;
			REQUIRE(writer.Write(source, AsBytes("allowed")));
			REQUIRE(writer.Move(source, moved));
			REQUIRE(writer.CreateDirectories(folder));
			REQUIRE(writer.Remove(moved));
			CHECK(admitted == 11);
			CHECK(reported == 4);
			const auto changes = watcher.Poll(3.0);
			REQUIRE(changes);
			CHECK(changes->empty());
			allowed = false;
			writer.SetMutationGuard({});
			REQUIRE(writer.Write(fresh, AsBytes("unbound")));
			CHECK(admitted == 11);
			CHECK(vfs.ReadText(fresh) == "unbound");
			writer.SetListener({});
		}

		TEST_CASE("AssetWriter: writes are marked known and reported to the listener")
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
			CHECK(events[0].ContentHash == XXH64(text));
			CHECK(events[1].ContentHash == 0);
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

		TEST_CASE("AssetWriter: a failed write reports nothing and dry runs never reach the watcher")
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

		TEST_CASE("AssetWriter: moving and removing a directory marks every file inside it known")
		{
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Models/Box.gltf", "{}");
			fixture.WriteProjectText("Assets/Models/Textures/Checker.png", "png");
			PollingFileWatcher watcher(fixture.GetVfs(), { .Root = fixture.ProjectPath("Assets"), .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start().has_value());
			AssetWriter writer(fixture.GetVfs());
			writer.SetWatcher(&watcher);
			std::vector<AssetWriteEvent> events;
			writer.SetListener([&events](const AssetWriteEvent& event)
			{
				events.push_back(event);
			});

			// The destination's parent directory is created by the move.
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Models"), fixture.ProjectPath("Assets/Levels/Track")).has_value());
			CHECK(fixture.GetVfs().Exists(fixture.ProjectPath("Assets/Levels/Track/Textures/Checker.png")));
			Result<std::vector<FileChange>> afterMove = watcher.Poll(1.0);
			REQUIRE(afterMove.has_value());
			CHECK(afterMove->empty());

			REQUIRE(writer.Remove(fixture.ProjectPath("Assets/Levels")).has_value());
			Result<std::vector<FileChange>> afterRemove = watcher.Poll(2.0);
			REQUIRE(afterRemove.has_value());
			CHECK(afterRemove->empty());
			REQUIRE(events.size() == 2);
			CHECK(events[0].From == fixture.ProjectPath("Assets/Models"));
			CHECK(events[1].Path == fixture.ProjectPath("Assets/Levels"));

			// A move between schemes is refused before anything happens.
			fixture.WriteProjectText("Assets/Red.material", "{}");
			CHECK_FALSE(writer.Move(fixture.ProjectPath("Assets/Red.material"), Test::ParseVfsPath("cache://Red.material")).has_value());
			CHECK(events.size() == 2);
		}

		TEST_CASE("AssetWriter: a case-only rename leaves the watcher nothing to report")
		{
			// The old spelling still resolves to the renamed file under the case policy (a case mismatch, not NotFound); the
			// watcher must record it as absent, or the next polls report the editor's own rename as a deletion.
			Test::AssetTestFixture fixture;
			fixture.WriteProjectText("Assets/Materials/red.material", "{}");
			fixture.WriteProjectText("Assets/Materials/red.material.meta", "{}");
			PollingFileWatcher watcher(fixture.GetVfs(), { .Root = fixture.ProjectPath("Assets"), .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start().has_value());
			AssetWriter writer(fixture.GetVfs());
			writer.SetWatcher(&watcher);

			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Materials/red.material"), fixture.ProjectPath("Assets/Materials/Red.material")).has_value());
			REQUIRE(writer.Move(fixture.ProjectPath("Assets/Materials/red.material.meta"), fixture.ProjectPath("Assets/Materials/Red.material.meta"))
					.has_value());
			CHECK(fixture.GetVfs().Exists(fixture.ProjectPath("Assets/Materials/Red.material")));
			for (const double seconds : { 1.0, 2.0 })
			{
				Result<std::vector<FileChange>> changes = watcher.Poll(seconds);
				REQUIRE(changes.has_value());
				CHECK(changes->empty());
			}
		}

		TEST_CASE("AssetWriter: the backup a mount keeps of a replaced file is reported and marked known")
		{
			// project:// is a native mount that keeps "<file>.bak" (§4.10), unlike the memory mounts of the other tests.
			const Test::TempDirectory directory("AssetWriterBackup");
			Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(directory.GetPath());
			REQUIRE_MESSAGE(mount.has_value(), mount.error().ToString());
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", std::move(*mount)).has_value());
			const VfsPath assets = Test::ParseVfsPath("project://Assets");
			const VfsPath material = Test::ParseVfsPath("project://Assets/Red.material");
			REQUIRE(vfs.CreateDirectories(assets).has_value());
			PollingFileWatcher watcher(vfs, { .Root = assets, .DebounceSeconds = 0.2 });
			REQUIRE(watcher.Start().has_value());
			AssetWriter writer(vfs);
			writer.SetWatcher(&watcher);
			std::vector<AssetWriteEvent> events;
			writer.SetListener([&events](const AssetWriteEvent& event)
			{
				events.push_back(event);
			});

			// A new file has no backup; replacing it leaves one.
			const std::string first = "{}";
			const std::string second = "{\"Format\": \"Material\"}";
			REQUIRE(writer.Write(material, AsBytes(first)).has_value());
			REQUIRE(writer.Write(material, AsBytes(second)).has_value());
			REQUIRE(events.size() == 2);
			CHECK(events[0].Backup.IsEmpty());
			CHECK(events[1].Backup == Test::ParseVfsPath("project://Assets/Red.material.bak"));
			CHECK(vfs.Exists(events[1].Backup));
			for (const double seconds : { 1.0, 2.0 })
			{
				Result<std::vector<FileChange>> changes = watcher.Poll(seconds);
				REQUIRE(changes.has_value());
				CHECK(changes->empty());
			}
		}
	}

}
