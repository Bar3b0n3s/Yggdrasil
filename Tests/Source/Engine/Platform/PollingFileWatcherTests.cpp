#include "TestsPCH.h"

#include "Engine/Platform/PollingFileWatcher.h"

#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Support/TempDirectory.h"

#include <atomic>
#include <thread>

namespace Engine {

	namespace {

		// A VFS with project:// on a MemoryMount holding an empty Assets folder, and helpers to change files in it. Times
		// are multiples of 1/8 s, exact in binary, so debounce boundaries are exact.
		class WatcherFixture
		{
		public:
			WatcherFixture()
			{
				REQUIRE(m_Vfs.Mount("project", CreateScope<MemoryMount>()).has_value());
				REQUIRE(m_Vfs.CreateDirectories(Path("project://Assets")).has_value());
			}

			[[nodiscard]] static VfsPath Path(std::string_view text)
			{
				Result<VfsPath> path = VfsPath::Parse(text);
				REQUIRE(path.has_value());
				return *path;
			}

			void Write(std::string_view path, std::string_view content)
			{
				REQUIRE(m_Vfs.WriteFileAtomic(Path(path), std::as_bytes(std::span(content))).has_value());
			}

			void Remove(std::string_view path)
			{
				REQUIRE(m_Vfs.Remove(Path(path)).has_value());
			}

			[[nodiscard]] VirtualFileSystem& GetVfs() { return m_Vfs; }
		private:
			VirtualFileSystem m_Vfs;
		};

	}

	static constexpr double Debounce = 0.25;

	static PollingFileWatcherSpecification MakeSpecification()
	{
		return PollingFileWatcherSpecification{ .Root = WatcherFixture::Path("project://Assets"), .DebounceSeconds = Debounce };
	}

	static std::vector<FileChange> PollOk(PollingFileWatcher& watcher, double now)
	{
		Result<std::vector<FileChange>> changes = watcher.Poll(now);
		REQUIRE_MESSAGE(changes.has_value(), changes.error().ToString());
		return std::move(*changes);
	}

	static FileChange Change(std::string_view path, FileChangeKind kind)
	{
		return FileChange{ .Path = WatcherFixture::Path(path), .Kind = kind };
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("PollingFileWatcher: create, modify and delete are reported once after debounce")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());
			CHECK(PollOk(watcher, 0.0).empty());

			// Created: seen at 1.0, reported at the first poll at least 0.25 s later, then never again.
			fixture.Write("project://Assets/Level.scene", "one");
			CHECK(PollOk(watcher, 1.0).empty());
			CHECK(PollOk(watcher, 1.125).empty());
			const std::vector<FileChange> created = PollOk(watcher, 1.25);
			REQUIRE(created.size() == 1);
			CHECK(created.front() == Change("project://Assets/Level.scene", FileChangeKind::Created));
			CHECK(PollOk(watcher, 1.5).empty());

			// Modified.
			fixture.Write("project://Assets/Level.scene", "two");
			CHECK(PollOk(watcher, 2.0).empty());
			const std::vector<FileChange> modified = PollOk(watcher, 2.25);
			REQUIRE(modified.size() == 1);
			CHECK(modified.front() == Change("project://Assets/Level.scene", FileChangeKind::Modified));
			CHECK(PollOk(watcher, 2.5).empty());

			// Deleted.
			fixture.Remove("project://Assets/Level.scene");
			CHECK(PollOk(watcher, 3.0).empty());
			const std::vector<FileChange> deleted = PollOk(watcher, 3.375);
			REQUIRE(deleted.size() == 1);
			CHECK(deleted.front() == Change("project://Assets/Level.scene", FileChangeKind::Deleted));
			CHECK(PollOk(watcher, 4.0).empty());
		}

		TEST_CASE("PollingFileWatcher: a file that keeps changing is reported once it settles")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Write("project://Assets/Script.luau", "a");
			CHECK(PollOk(watcher, 0.0).empty());
			fixture.Write("project://Assets/Script.luau", "ab");
			CHECK(PollOk(watcher, 0.125).empty()); // changed again: the debounce restarts at 0.125
			CHECK(PollOk(watcher, 0.25).empty());
			const std::vector<FileChange> settled = PollOk(watcher, 0.375);
			REQUIRE(settled.size() == 1);
			CHECK(settled.front() == Change("project://Assets/Script.luau", FileChangeKind::Created));
		}

		TEST_CASE("PollingFileWatcher: only net content changes are reported")
		{
			WatcherFixture fixture;
			fixture.Write("project://Assets/Kept.material", "same");
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			// Rewritten with the same bytes: the modification time changes, the content hash does not.
			fixture.Write("project://Assets/Kept.material", "same");
			// Created and deleted within the debounce.
			fixture.Write("project://Assets/Temporary.tmp", "x");
			CHECK(PollOk(watcher, 0.0).empty());
			fixture.Remove("project://Assets/Temporary.tmp");
			CHECK(PollOk(watcher, 0.125).empty());
			CHECK(PollOk(watcher, 1.0).empty());

			// Changed and restored before the debounce expired.
			fixture.Write("project://Assets/Kept.material", "different");
			CHECK(PollOk(watcher, 2.0).empty());
			fixture.Write("project://Assets/Kept.material", "same");
			CHECK(PollOk(watcher, 2.125).empty());
			CHECK(PollOk(watcher, 3.0).empty());
		}

		TEST_CASE("PollingFileWatcher: MarkKnown keeps the engine's own writes from coming back")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Write("project://Assets/Saved.scene", "by the editor");
			REQUIRE(watcher.MarkKnown(WatcherFixture::Path("project://Assets/Saved.scene")).has_value());
			CHECK(PollOk(watcher, 0.0).empty());
			CHECK(PollOk(watcher, 1.0).empty());

			// A later external change is still reported.
			fixture.Write("project://Assets/Saved.scene", "by another program");
			CHECK(PollOk(watcher, 2.0).empty());
			CHECK(PollOk(watcher, 2.25).size() == 1);

			const Status outside = watcher.MarkKnown(WatcherFixture::Path("project://Other/File.txt"));
			REQUIRE_FALSE(outside.has_value());
			CHECK(outside.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PollingFileWatcher: changes come sorted by path and nested files count")
		{
			WatcherFixture fixture;
			REQUIRE(fixture.GetVfs().CreateDirectories(WatcherFixture::Path("project://Assets/Textures")).has_value());
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Write("project://Assets/c.txt", "c");
			fixture.Write("project://Assets/Textures/b.png", "b");
			fixture.Write("project://Assets/a.txt", "a");
			REQUIRE(fixture.GetVfs().CreateDirectories(WatcherFixture::Path("project://Assets/Empty")).has_value());
			CHECK(PollOk(watcher, 0.0).empty());
			const std::vector<FileChange> changes = PollOk(watcher, 0.25);
			REQUIRE(changes.size() == 3); // directories are not reported
			CHECK(changes[0] == Change("project://Assets/Textures/b.png", FileChangeKind::Created));
			CHECK(changes[1] == Change("project://Assets/a.txt", FileChangeKind::Created));
			CHECK(changes[2] == Change("project://Assets/c.txt", FileChangeKind::Created));
		}

		TEST_CASE("PollingFileWatcher: Poll before Start is InvalidState and a missing root is NotFound")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			const Result<std::vector<FileChange>> early = watcher.Poll(0.0);
			REQUIRE_FALSE(early.has_value());
			CHECK(early.error().GetCode() == ErrorCode::InvalidState);

			PollingFileWatcher missing(fixture.GetVfs(), { .Root = WatcherFixture::Path("project://Missing") });
			const Status started = missing.Start();
			REQUIRE_FALSE(started.has_value());
			CHECK(started.error().GetCode() == ErrorCode::NotFound);

			CHECK(FileChangeKindToString(FileChangeKind::Created) == "Created");
			CHECK(FileChangeKindToString(FileChangeKind::Modified) == "Modified");
			CHECK(FileChangeKindToString(FileChangeKind::Deleted) == "Deleted");
		}

		TEST_CASE("PollingFileWatcher: deleted and recreated is Modified, or nothing for identical content")
		{
			WatcherFixture fixture;
			fixture.Write("project://Assets/Changed.txt", "old");
			fixture.Write("project://Assets/Same.txt", "same");
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Remove("project://Assets/Changed.txt");
			fixture.Remove("project://Assets/Same.txt");
			CHECK(PollOk(watcher, 0.0).empty());
			fixture.Write("project://Assets/Changed.txt", "new");
			fixture.Write("project://Assets/Same.txt", "same");
			CHECK(PollOk(watcher, 0.125).empty());
			const std::vector<FileChange> changes = PollOk(watcher, 0.375);
			REQUIRE(changes.size() == 1);
			CHECK(changes.front() == Change("project://Assets/Changed.txt", FileChangeKind::Modified));
			CHECK(PollOk(watcher, 1.0).empty());
		}

		TEST_CASE("PollingFileWatcher: a zero debounce reports a change on the poll that sees it")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), { .Root = WatcherFixture::Path("project://Assets"), .DebounceSeconds = 0.0 });
			REQUIRE(watcher.Start().has_value());
			CHECK(watcher.GetSpecification().DebounceSeconds == 0.0);
			CHECK(watcher.GetSpecification().Root == WatcherFixture::Path("project://Assets"));

			fixture.Write("project://Assets/Now.txt", "now");
			const std::vector<FileChange> created = PollOk(watcher, 0.0);
			REQUIRE(created.size() == 1);
			CHECK(created.front() == Change("project://Assets/Now.txt", FileChangeKind::Created));
			CHECK(PollOk(watcher, 0.0).empty());
		}

		TEST_CASE("PollingFileWatcher: Start takes a new baseline and drops pending changes")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Write("project://Assets/Early.txt", "early");
			CHECK(PollOk(watcher, 0.0).empty()); // pending
			REQUIRE(watcher.Start().has_value());
			CHECK(PollOk(watcher, 1.0).empty()); // part of the new baseline, so never reported

			// Changes after the new baseline are reported against it.
			fixture.Write("project://Assets/Early.txt", "changed");
			CHECK(PollOk(watcher, 2.0).empty());
			const std::vector<FileChange> modified = PollOk(watcher, 2.25);
			REQUIRE(modified.size() == 1);
			CHECK(modified.front() == Change("project://Assets/Early.txt", FileChangeKind::Modified));
		}

		TEST_CASE("PollingFileWatcher: MarkKnown drops pending changes and records removed files")
		{
			WatcherFixture fixture;
			fixture.Write("project://Assets/Removed.scene", "doomed");
			fixture.Write("project://Assets/Edited.scene", "v1");
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			// An external change is pending when the engine overwrites the file and records it: nothing is reported.
			fixture.Write("project://Assets/Edited.scene", "v2 from another program");
			CHECK(PollOk(watcher, 0.0).empty());
			fixture.Write("project://Assets/Edited.scene", "v3 from the editor");
			REQUIRE(watcher.MarkKnown(WatcherFixture::Path("project://Assets/Edited.scene")).has_value());

			// The engine removes a file and records that it no longer exists: no Deleted comes back.
			fixture.Remove("project://Assets/Removed.scene");
			REQUIRE(watcher.MarkKnown(WatcherFixture::Path("project://Assets/Removed.scene")).has_value());
			// A path that never existed is fine as well.
			REQUIRE(watcher.MarkKnown(WatcherFixture::Path("project://Assets/Never.scene")).has_value());

			CHECK(PollOk(watcher, 0.125).empty());
			CHECK(PollOk(watcher, 1.0).empty());
			CHECK(PollOk(watcher, 2.0).empty());
		}

		TEST_CASE("PollingFileWatcher: MarkKnown rejects the root and directories")
		{
			WatcherFixture fixture;
			REQUIRE(fixture.GetVfs().CreateDirectories(WatcherFixture::Path("project://Assets/Folder")).has_value());
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			const Status root = watcher.MarkKnown(WatcherFixture::Path("project://Assets"));
			REQUIRE_FALSE(root.has_value());
			CHECK(root.error().GetCode() == ErrorCode::InvalidArgument);

			const Status sibling = watcher.MarkKnown(WatcherFixture::Path("project://AssetsOther/File.txt"));
			REQUIRE_FALSE(sibling.has_value());
			CHECK(sibling.error().GetCode() == ErrorCode::InvalidArgument);

			const Status folder = watcher.MarkKnown(WatcherFixture::Path("project://Assets/Folder"));
			REQUIRE_FALSE(folder.has_value());
			CHECK(folder.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("PollingFileWatcher: a vanished root fails Poll and watching resumes when it returns")
		{
			WatcherFixture fixture;
			fixture.Write("project://Assets/Kept.txt", "kept");
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Remove("project://Assets");
			const Result<std::vector<FileChange>> vanished = watcher.Poll(0.0);
			REQUIRE_FALSE(vanished.has_value());
			CHECK(vanished.error().GetCode() == ErrorCode::NotFound);

			// The failed poll changed nothing: the restored file is not a change, a new one is.
			REQUIRE(fixture.GetVfs().CreateDirectories(WatcherFixture::Path("project://Assets")).has_value());
			fixture.Write("project://Assets/Kept.txt", "kept");
			fixture.Write("project://Assets/New.txt", "new");
			CHECK(PollOk(watcher, 1.0).empty());
			const std::vector<FileChange> changes = PollOk(watcher, 1.25);
			REQUIRE(changes.size() == 1);
			CHECK(changes.front() == Change("project://Assets/New.txt", FileChangeKind::Created));
		}

		TEST_CASE("PollingFileWatcher: files outside the root are not watched")
		{
			WatcherFixture fixture;
			REQUIRE(fixture.GetVfs().CreateDirectories(WatcherFixture::Path("project://Library")).has_value());
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			fixture.Write("project://Library/Autosave.scene", "autosave");
			fixture.Write("project://Top.txt", "top");
			CHECK(PollOk(watcher, 0.0).empty());
			CHECK(PollOk(watcher, 1.0).empty());
		}

		TEST_CASE("PollingFileWatcher: a native directory reports create, modify and delete")
		{
			// Real files, whose sizes differ between writes: two writes of the same size within one tick of a coarse host
			// clock may share a modification time, and the watcher compares size and time before it reads anything.
			Test::TempDirectory directory("PollingFileWatcher");
			std::error_code error;
			std::filesystem::create_directories(directory / "Assets", error);
			REQUIRE_FALSE(error);
			Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(directory.GetPath(), MountAccess::ReadWrite,
				AtomicWriteOptions{ .KeepBackup = false });
			REQUIRE(mount.has_value());
			VirtualFileSystem vfs;
			REQUIRE(vfs.Mount("project", std::move(*mount)).has_value());

			PollingFileWatcher watcher(vfs, MakeSpecification());
			REQUIRE(watcher.Start().has_value());
			const VfsPath file = WatcherFixture::Path("project://Assets/Native.txt");

			REQUIRE(vfs.WriteFileAtomic(file, AsBytes("one")).has_value());
			CHECK(PollOk(watcher, 0.0).empty());
			CHECK(PollOk(watcher, 0.25) == std::vector<FileChange>{ Change("project://Assets/Native.txt", FileChangeKind::Created) });

			REQUIRE(vfs.WriteFileAtomic(file, AsBytes("second")).has_value());
			CHECK(PollOk(watcher, 1.0).empty());
			CHECK(PollOk(watcher, 1.25) == std::vector<FileChange>{ Change("project://Assets/Native.txt", FileChangeKind::Modified) });

			REQUIRE(vfs.Remove(file).has_value());
			CHECK(PollOk(watcher, 2.0).empty());
			CHECK(PollOk(watcher, 2.25) == std::vector<FileChange>{ Change("project://Assets/Native.txt", FileChangeKind::Deleted) });
		}

		TEST_CASE("PollingFileWatcher: MarkKnown on another thread during Start and Poll never echoes the engine's writes")
		{
			WatcherFixture fixture;
			PollingFileWatcher watcher(fixture.GetVfs(), MakeSpecification());
			REQUIRE(watcher.Start().has_value());

			// The writer stands in for AssetWriter on the main thread: it creates files, then removes every third one, and
			// records each change with MarkKnown right after making it. Each file changes only once per phase, so the scan that
			// overlaps a change is the last one to see that file before the end: a scan that merged a listing older than the
			// MarkKnown it overlapped would leave a stale state behind. This thread scans meanwhile, at a constant time so that
			// no debounce expires while the writer runs. Whatever the interleaving, every change was recorded as known, so
			// nothing may be reported afterwards.
			constexpr size_t FileCount = 300;
			std::vector<VfsPath> paths;
			for (size_t index = 0; index < FileCount; ++index)
				paths.push_back(WatcherFixture::Path(std::format("project://Assets/File{:03}.txt", index)));
			std::atomic<bool> isWriting = true;
			Status writerStatus;
			std::thread writer([&fixture, &watcher, &paths, &isWriting, &writerStatus]()
			{
				for (size_t index = 0; index < paths.size() && writerStatus.has_value(); ++index)
				{
					writerStatus = fixture.GetVfs().WriteFileAtomic(paths[index], AsBytes(std::format("content {}", index)));
					if (writerStatus)
						writerStatus = watcher.MarkKnown(paths[index]);
				}
				for (size_t index = 0; index < paths.size() && writerStatus.has_value(); index += 3)
				{
					writerStatus = fixture.GetVfs().Remove(paths[index]);
					if (writerStatus)
						writerStatus = watcher.MarkKnown(paths[index]);
				}
				isWriting = false;
			});

			Status scanStatus;
			for (int round = 0; isWriting && scanStatus.has_value(); ++round)
			{
				if (round % 2 == 0)
				{
					scanStatus = watcher.Start();
				}
				else
				{
					Result<std::vector<FileChange>> changes = watcher.Poll(0.0);
					scanStatus = changes.has_value() ? Status() : Status(std::unexpected(changes.error()));
				}
			}
			writer.join();

			REQUIRE_MESSAGE(writerStatus.has_value(), writerStatus.error().ToString());
			REQUIRE_MESSAGE(scanStatus.has_value(), scanStatus.error().ToString());
			CHECK(PollOk(watcher, 10.0).empty());
			CHECK(PollOk(watcher, 20.0).empty());
		}
	}

}
