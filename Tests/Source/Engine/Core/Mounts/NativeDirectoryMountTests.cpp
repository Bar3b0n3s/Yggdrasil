#include "TestsPCH.h"

#include "Engine/Core/Mounts/NativeDirectoryMount.h"

#include "Support/TempDirectory.h"

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
		TEST_CASE("NativeDirectoryMount: Create rejects a missing root and a file" * doctest::skip(true))
		{
			Test::TempDirectory directory("NativeMount");
			CHECK(ErrorCodeOf(NativeDirectoryMount::Create(directory / "Missing")) == ErrorCode::NotFound);

			REQUIRE(FileSystem::WriteFileAtomic(directory / "File.txt", AsBytes("x")).has_value());
			CHECK(ErrorCodeOf(NativeDirectoryMount::Create(directory / "File.txt")) == ErrorCode::InvalidArgument);

			const Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(directory.GetPath());
			REQUIRE(mount.has_value());
			CHECK((*mount)->GetRoot() == directory.GetPath());
		}

		TEST_CASE("NativeDirectoryMount: reads and writes below its root with atomic writes" * doctest::skip(true))
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

		TEST_CASE("NativeDirectoryMount: read-only access rejects writes" * doctest::skip(true))
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

		TEST_CASE("NativeDirectoryMount: a missing root fails every call with NotFound" * doctest::skip(true))
		{
			Test::TempDirectory directory("NativeMount");
			NativeDirectoryMount mount(directory / "Missing");
			CHECK(ErrorCodeOf(mount.ReadFile(Path("project://A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.List(Path("project://"), false)) == ErrorCode::NotFound);
		}

		TEST_CASE("NativeDirectoryMount: a new name that differs from a file only in case never overwrites it" * doctest::skip(true))
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

		TEST_CASE("NativeDirectoryMount: a case-only Move renames the file on every host" * doctest::skip(true))
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

		TEST_CASE("NativeDirectoryMount: an open stream reads its snapshot while the file is replaced or removed" * doctest::skip(true))
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

		TEST_CASE("NativeDirectoryMount: write options control the backup and inject failures" * doctest::skip(true))
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
	}

}
