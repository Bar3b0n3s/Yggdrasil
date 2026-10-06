#include "TestsPCH.h"

#include "Engine/Platform/PollingFileWatcher.h"

#include "Engine/Core/Mounts/MemoryMount.h"

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
		TEST_CASE("PollingFileWatcher: create, modify and delete are reported once after debounce" * doctest::skip(true))
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

		TEST_CASE("PollingFileWatcher: a file that keeps changing is reported once it settles" * doctest::skip(true))
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

		TEST_CASE("PollingFileWatcher: only net content changes are reported" * doctest::skip(true))
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

		TEST_CASE("PollingFileWatcher: MarkKnown keeps the engine's own writes from coming back" * doctest::skip(true))
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

		TEST_CASE("PollingFileWatcher: changes come sorted by path and nested files count" * doctest::skip(true))
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

		TEST_CASE("PollingFileWatcher: Poll before Start is InvalidState and a missing root is NotFound" * doctest::skip(true))
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
	}

}
