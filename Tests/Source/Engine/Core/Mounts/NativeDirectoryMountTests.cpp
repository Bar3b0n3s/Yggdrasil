#include "TestsPCH.h"

#include "Engine/Core/Mounts/NativeDirectoryMount.h"

#include "Support/TempDirectory.h"

#include <chrono>
#include <latch>
#include <system_error>
#include <thread>

namespace Engine {

	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	static VfsPath Path(std::string_view text)
	{
		Result<VfsPath> path = VfsPath::Parse(text);
		REQUIRE(path.has_value());
		return std::move(*path);
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("NativeDirectoryMount: Create rejects a missing root and a file")
		{
			Test::TempDirectory directory("NativeMount");
			CHECK(ErrorCodeOf(NativeDirectoryMount::Create(directory / "Missing")) == ErrorCode::NotFound);

			REQUIRE(FileSystem::WriteFileAtomic(directory / "File.txt", AsBytes("x")).has_value());
			CHECK(ErrorCodeOf(NativeDirectoryMount::Create(directory / "File.txt")) == ErrorCode::InvalidArgument);

			const Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(directory.GetPath());
			REQUIRE(mount.has_value());
			CHECK((*mount)->GetRoot() == directory.GetPath());
		}

		TEST_CASE("NativeDirectoryMount: reads and writes below its root with atomic writes")
		{
			Test::TempDirectory directory("NativeMount");
			NativeDirectoryMount mount(directory.GetPath());

			REQUIRE(mount.CreateDirectories(Path("project://Assets/Scenes")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/Scenes/Level1.scene"), AsBytes("first")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/Scenes/Level1.scene"), AsBytes("second")).has_value());

			CHECK(FileSystem::ReadText(directory / "Assets/Scenes/Level1.scene") == std::string("second"));
			CHECK(FileSystem::ReadText(directory / "Assets/Scenes/Level1.scene.bak") == std::string("first"));

			const Result<Buffer> bytes = mount.ReadFile(Path("project://Assets/Scenes/Level1.scene"));
			REQUIRE(bytes.has_value());
			CHECK(AsStringView(*bytes) == "second");

			REQUIRE(mount.Move(Path("project://Assets/Scenes/Level1.scene"), Path("project://Assets/Scenes/Renamed.scene")).has_value());
			CHECK(FileSystem::Exists(directory / "Assets/Scenes/Renamed.scene"));
			REQUIRE(mount.Remove(Path("project://Assets")).has_value());
			CHECK_FALSE(FileSystem::Exists(directory / "Assets"));
		}

		TEST_CASE("NativeDirectoryMount: read-only access rejects writes")
		{
			Test::TempDirectory directory("NativeMount");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "A.txt", AsBytes("a")).has_value());
			NativeDirectoryMount mount(directory.GetPath(), MountAccess::ReadOnly);

			CHECK(mount.GetAccess() == MountAccess::ReadOnly);
			CHECK(mount.ReadFile(Path("engine://A.txt")).has_value());
			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("engine://A.txt"), AsBytes("b"))) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.Remove(Path("engine://A.txt"))) == ErrorCode::PermissionDenied);
			CHECK(FileSystem::ReadText(directory / "A.txt") == std::string("a"));
		}

		TEST_CASE("NativeDirectoryMount: a missing root fails every call with NotFound")
		{
			Test::TempDirectory directory("NativeMount");
			NativeDirectoryMount mount(directory / "Missing");
			CHECK(ErrorCodeOf(mount.ReadFile(Path("project://A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.Open(Path("project://A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.GetInfo(Path("project://A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.List(Path("project://"), false)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("project://A.txt"), AsBytes("a"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Assets"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.Remove(Path("project://A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.Move(Path("project://A.txt"), Path("project://B.txt"))) == ErrorCode::NotFound);

			// A deleted project root is never re-created behind the caller's back.
			CHECK_FALSE(FileSystem::Exists(directory / "Missing"));
		}

		TEST_CASE("NativeDirectoryMount: GetInfo reports a modification time that changes when the file is rewritten")
		{
			Test::TempDirectory directory("NativeMountInfo");
			NativeDirectoryMount mount(directory.GetPath());
			const VfsPath path = Path("project://Level1.scene");
			REQUIRE(mount.WriteFileAtomic(path, AsBytes("first")).has_value());

			// A day in the past, so the rewrite below gets a different time at any timestamp resolution, without sleeping.
			std::error_code error;
			std::filesystem::last_write_time(directory / "Level1.scene",
				std::filesystem::file_time_type::clock::now() - std::chrono::hours(24), error);
			REQUIRE_FALSE(error);

			const Result<FileInfo> before = mount.GetInfo(path);
			const Result<FileInfo> again = mount.GetInfo(path);
			REQUIRE(before.has_value());
			REQUIRE(again.has_value());
			CHECK(before->ModificationTime == again->ModificationTime);
			CHECK(before->Size == 5);

			REQUIRE(mount.WriteFileAtomic(path, AsBytes("second")).has_value());
			const Result<FileInfo> after = mount.GetInfo(path);
			REQUIRE(after.has_value());
			CHECK(after->ModificationTime != before->ModificationTime);
			CHECK(after->Size == 6);
		}

		TEST_CASE("NativeDirectoryMount: a new name that differs from a file only in case never overwrites it")
		{
			Test::TempDirectory directory("NativeMountCase");
			REQUIRE(FileSystem::CreateDirectories(directory / "Assets").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/Level1.scene", AsBytes("original")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/Other.scene", AsBytes("other")).has_value());
			NativeDirectoryMount mount(directory.GetPath());

			// On a case-insensitive host the write would replace Level1.scene, on a case-sensitive one it would add a
			// second file; the case policy makes both hosts refuse it the same way.
			const Status write = mount.WriteFileAtomic(Path("project://Assets/level1.scene"), AsBytes("replacement"));
			REQUIRE_FALSE(write.has_value());
			CHECK(write.error().GetCode() == ErrorCode::Validation);
			CHECK(write.error().GetMessageText().contains("Level1.scene"));
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Assets/LEVEL1.scene"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(mount.Move(Path("project://Assets/Other.scene"), Path("project://Assets/level1.scene")))
				== ErrorCode::Validation);

			CHECK(FileSystem::ReadText(directory / "Assets/Level1.scene") == std::string("original"));
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory / "Assets");
			REQUIRE(entries.has_value());
			CHECK(entries->size() == 2);
		}

		TEST_CASE("NativeDirectoryMount: a new name the host resolves to an existing entry never replaces it")
		{
			// Names that no directory entry spells, even ignoring ASCII case, which some hosts still resolve to an existing
			// entry: non-ASCII case folding (NTFS, APFS), Unicode normalization (APFS, HFS+) and 8.3 short names (Windows,
			// where the volume keeps them). Each subcase asks the host whether it aliases the two names: if it does, every
			// mutation that would use the new name is Validation and the existing entry keeps its content; if it does
			// not (Linux), the new name is a separate entry, as on MemoryMount.
			std::string existing;
			std::string alias;
			SUBCASE("non-ASCII letter case")
			{
				existing = "\xc3\x89t\xc3\xa9.txt"; // U+00C9 E with acute, 't', U+00E9 e with acute
				alias = "\xc3\xa9t\xc3\xa9.txt";    // the same name with a lower-case first letter
			}
			SUBCASE("Unicode normalization")
			{
				existing = "Caf\xc3\xa9.txt"; // precomposed U+00E9 (NFC)
				alias = "Cafe\xcc\x81.txt";   // 'e' and the combining U+0301 (NFD)
			}
			SUBCASE("8.3 short name")
			{
				existing = "LongFileNameForAliasTest.txt";
				alias = "LONGFI~1.TXT";
			}

			Test::TempDirectory directory("NativeMountAlias");
			REQUIRE(FileSystem::WriteFileAtomic(directory / existing, AsBytes("original")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Other.txt", AsBytes("other")).has_value());
			const bool hostAliases = FileSystem::Exists(directory / alias);
			NativeDirectoryMount mount(directory.GetPath());
			const VfsPath aliasPath = Path("project://" + alias);

			const Status written = mount.WriteFileAtomic(aliasPath, AsBytes("replacement"));
			const Status created = mount.CreateDirectories(Path("project://" + alias + "/Nested"));
			const Status moved = mount.Move(Path("project://Other.txt"), aliasPath);
			CHECK(FileSystem::ReadText(directory / existing) == std::string("original"));

			INFO("the host aliases '", alias, "' to '", existing, "': ", hostAliases);
			if (hostAliases)
			{
				REQUIRE_FALSE(written.has_value());
				CHECK(written.error().GetCode() == ErrorCode::Validation);
				CHECK(written.error().GetMessageText().contains(existing));
				CHECK(ErrorCodeOf(created) == ErrorCode::Validation);
				CHECK(ErrorCodeOf(moved) == ErrorCode::Validation);
				CHECK(FileSystem::ReadText(directory / "Other.txt") == std::string("other"));
				const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory.GetPath(), true);
				REQUIRE(entries.has_value());
				CHECK(entries->size() == 2);
			}
			else
			{
				// The write created the new file, so the directory creation finds a file in the way and the move finds
				// an existing destination; the original entry is untouched.
				CHECK(written.has_value());
				CHECK(ErrorCodeOf(created) == ErrorCode::AlreadyExists);
				CHECK(ErrorCodeOf(moved) == ErrorCode::AlreadyExists);
				CHECK(FileSystem::ReadText(directory / alias) == std::string("replacement"));
			}
		}

		TEST_CASE("NativeDirectoryMount: a case-only Move renames the file on every host")
		{
			Test::TempDirectory directory("NativeMountRename");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "level1.scene", AsBytes("content")).has_value());
			NativeDirectoryMount mount(directory.GetPath());

			REQUIRE(mount.Move(Path("project://level1.scene"), Path("project://Level1.scene")).has_value());
			CHECK(mount.ReadFile(Path("project://Level1.scene")).has_value());
			CHECK(ErrorCodeOf(mount.ReadFile(Path("project://level1.scene"))) == ErrorCode::Validation);
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory.GetPath());
			REQUIRE(entries.has_value());
			REQUIRE(entries->size() == 1);
			CHECK((*entries)[0].filename() == "Level1.scene");
		}

		TEST_CASE("NativeDirectoryMount: a case-only Move renames a directory on every host")
		{
			Test::TempDirectory directory("NativeMountRenameDirectory");
			REQUIRE(FileSystem::CreateDirectories(directory / "assets").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "assets/Level1.scene", AsBytes("content")).has_value());
			NativeDirectoryMount mount(directory.GetPath());

			REQUIRE(mount.Move(Path("project://assets"), Path("project://Assets")).has_value());
			CHECK(mount.ReadFile(Path("project://Assets/Level1.scene")).has_value());
			CHECK(ErrorCodeOf(mount.ReadFile(Path("project://assets/Level1.scene"))) == ErrorCode::Validation);
			const Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory.GetPath());
			REQUIRE(entries.has_value());
			REQUIRE(entries->size() == 1);
			CHECK((*entries)[0].filename() == "Assets");
		}

		TEST_CASE("NativeDirectoryMount: List returns sorted VFS paths with their kinds")
		{
			Test::TempDirectory directory("NativeMountList");
			REQUIRE(FileSystem::CreateDirectories(directory / "Assets/b").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/b/c.txt", AsBytes("ccc")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/a.txt", AsBytes("a")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/B.txt", AsBytes("bb")).has_value());
			NativeDirectoryMount mount(directory.GetPath());

			const Result<std::vector<VfsEntry>> listed = mount.List(Path("project://Assets"), true);
			REQUIRE(listed.has_value());
			std::vector<std::string> paths;
			for (const VfsEntry& entry : *listed)
				paths.push_back(entry.Path.ToString());
			CHECK(paths
				== std::vector<std::string>{
					"project://Assets/B.txt",
					"project://Assets/a.txt",
					"project://Assets/b",
					"project://Assets/b/c.txt",
				});
			REQUIRE(listed->size() == 4);
			CHECK((*listed)[0].Info.Size == 2);
			CHECK((*listed)[2].Info.IsDirectory);
			CHECK((*listed)[3].Info.Size == 3);
			CHECK(ErrorCodeOf(mount.List(Path("project://Assets/a.txt"), false)) == ErrorCode::Io);
			CHECK(ErrorCodeOf(mount.List(Path("project://assets"), false)) == ErrorCode::Validation);
		}

		TEST_CASE("NativeDirectoryMount: CreateDirectories applies the case policy to existing components")
		{
			Test::TempDirectory directory("NativeMountDirectories");
			NativeDirectoryMount mount(directory.GetPath());
			REQUIRE(mount.CreateDirectories(Path("project://Assets/Scenes")).has_value());
			CHECK(mount.CreateDirectories(Path("project://Assets/Scenes")).has_value());
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://assets/Prefabs"))) == ErrorCode::Validation);
			const Result<std::vector<std::filesystem::path>> top = FileSystem::ListDirectory(directory.GetPath(), true);
			REQUIRE(top.has_value());
			CHECK(top->size() == 2); // Assets and Assets/Scenes: nothing was created for the other spelling

			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/File.txt"), AsBytes("x")).has_value());
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Assets/File.txt"))) == ErrorCode::AlreadyExists);
			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("project://Missing/File.txt"), AsBytes("x"))) == ErrorCode::NotFound);
		}

		TEST_CASE("NativeDirectoryMount: an open stream reads its snapshot while the file is replaced or removed")
		{
			Test::TempDirectory directory("NativeMountStream");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Clip.ogg", AsBytes("original")).has_value());
			NativeDirectoryMount mount(directory.GetPath());

			Result<Scope<IFileStream>> stream = mount.Open(Path("project://Clip.ogg"));
			REQUIRE(stream.has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Clip.ogg"), AsBytes("replaced")).has_value());
			REQUIRE(mount.Move(Path("project://Clip.ogg"), Path("project://Moved.ogg")).has_value());
			REQUIRE(mount.Remove(Path("project://Moved.ogg")).has_value());

			std::array<std::byte, 8> bytes{};
			CHECK((*stream)->Read(bytes) == size_t{ 8 });
			CHECK(AsStringView(bytes) == "original");
		}

		TEST_CASE("NativeDirectoryMount: write options control the backup and inject failures")
		{
			Test::TempDirectory directory("NativeMountOptions");
			AtomicWriteOptions noBackup;
			noBackup.KeepBackup = false;
			NativeDirectoryMount cache(directory.GetPath(), MountAccess::ReadWrite, noBackup);
			CHECK_FALSE(cache.GetWriteOptions().KeepBackup);

			REQUIRE(cache.WriteFileAtomic(Path("cache://Cooked.bin"), AsBytes("first")).has_value());
			REQUIRE(cache.WriteFileAtomic(Path("cache://Cooked.bin"), AsBytes("second")).has_value());
			CHECK(FileSystem::ReadText(directory / "Cooked.bin") == std::string("second"));
			CHECK_FALSE(FileSystem::Exists(directory / "Cooked.bin.bak"));

			AtomicWriteOptions failing;
			failing.InjectFailure = AtomicWriteStep::Replace;
			NativeDirectoryMount broken(directory.GetPath(), MountAccess::ReadWrite, failing);
			CHECK(ErrorCodeOf(broken.WriteFileAtomic(Path("cache://Cooked.bin"), AsBytes("third"))) == ErrorCode::Io);
			CHECK(FileSystem::ReadText(directory / "Cooked.bin") == std::string("second"));
		}

		TEST_CASE("NativeDirectoryMount: concurrent writes of two spellings of one name create exactly one file")
		{
			// Each call's case check and write are atomic with respect to the other calls on the mount (IMount), so of two
			// racing writes of "level1.scene" and "Level1.scene" exactly one succeeds and the other is the case policy's
			// Validation error, on every host.
			Test::TempDirectory directory("NativeMountRace");
			NativeDirectoryMount mount(directory.GetPath());
			constexpr int Rounds = 64;
			for (int round = 0; round < Rounds; ++round)
			{
				const std::string folder = std::format("project://Round{}", round);
				REQUIRE(mount.CreateDirectories(Path(folder)).has_value());
				const VfsPath lower = Path(folder + "/level1.scene");
				const VfsPath upper = Path(folder + "/Level1.scene");

				std::latch start(2);
				Status lowerResult;
				Status upperResult;
				std::thread lowerWriter([&mount, &start, &lower, &lowerResult]()
				{
					start.arrive_and_wait();
					lowerResult = mount.WriteFileAtomic(lower, AsBytes("lower"));
				});
				std::thread upperWriter([&mount, &start, &upper, &upperResult]()
				{
					start.arrive_and_wait();
					upperResult = mount.WriteFileAtomic(upper, AsBytes("upper"));
				});
				lowerWriter.join();
				upperWriter.join();

				INFO("round ", round);
				CHECK(lowerResult.has_value() != upperResult.has_value());
				const std::optional<ErrorCode> failure = lowerResult.has_value() ? ErrorCodeOf(upperResult) : ErrorCodeOf(lowerResult);
				CHECK(failure == ErrorCode::Validation);
				const Result<std::vector<VfsEntry>> entries = mount.List(Path(folder), false);
				REQUIRE(entries.has_value());
				CHECK(entries->size() == 1);
			}
		}
	}

}
