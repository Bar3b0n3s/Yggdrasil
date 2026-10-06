#include "TestsPCH.h"

#include "Engine/Core/Mounts/MemoryMount.h"

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
		TEST_CASE("MemoryMount: ModificationTime changes exactly when a file is written" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.WriteFileAtomic(Path("project://A.txt"), AsBytes("1")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://B.txt"), AsBytes("2")).has_value());

			const Result<FileInfo> first = mount.GetInfo(Path("project://A.txt"));
			REQUIRE(first.has_value());
			CHECK(first->Size == 1);

			REQUIRE(mount.WriteFileAtomic(Path("project://B.txt"), AsBytes("22")).has_value());
			const Result<FileInfo> unchanged = mount.GetInfo(Path("project://A.txt"));
			REQUIRE(unchanged.has_value());
			CHECK(unchanged->ModificationTime == first->ModificationTime);

			REQUIRE(mount.WriteFileAtomic(Path("project://A.txt"), AsBytes("11")).has_value());
			const Result<FileInfo> changed = mount.GetInfo(Path("project://A.txt"));
			REQUIRE(changed.has_value());
			CHECK(changed->ModificationTime != first->ModificationTime);
			CHECK(changed->Size == 2);
			CHECK(mount.GetMutationCount() == 4);
		}

		TEST_CASE("MemoryMount: writes need an existing parent directory" * doctest::skip(true))
		{
			MemoryMount mount;
			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("project://Assets/A.txt"), AsBytes("a"))) == ErrorCode::NotFound);
			REQUIRE(mount.CreateDirectories(Path("project://Assets/Deep/Er")).has_value());
			CHECK(mount.WriteFileAtomic(Path("project://Assets/Deep/Er/A.txt"), AsBytes("a")).has_value());

			const Result<FileInfo> directory = mount.GetInfo(Path("project://Assets/Deep"));
			REQUIRE(directory.has_value());
			CHECK(directory->IsDirectory);
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Assets/Deep/Er/A.txt"))) == ErrorCode::AlreadyExists);
		}

		TEST_CASE("MemoryMount: read-only access rejects every mutation" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.WriteFileAtomic(Path("project://A.txt"), AsBytes("a")).has_value());
			mount.SetAccess(MountAccess::ReadOnly);
			CHECK(mount.GetAccess() == MountAccess::ReadOnly);

			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("project://A.txt"), AsBytes("b"))) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Dir"))) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.Remove(Path("project://A.txt"))) == ErrorCode::PermissionDenied);
			CHECK(ErrorCodeOf(mount.Move(Path("project://A.txt"), Path("project://B.txt"))) == ErrorCode::PermissionDenied);
			CHECK(mount.ReadFile(Path("project://A.txt")).has_value());
		}

		TEST_CASE("MemoryMount: case mismatch is a validation error" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.CreateDirectories(Path("project://Assets")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("{}")).has_value());

			const Result<Buffer> wrong = mount.ReadFile(Path("project://Assets/LEVEL1.scene"));
			REQUIRE_FALSE(wrong.has_value());
			CHECK(wrong.error().GetCode() == ErrorCode::Validation);
			CHECK(wrong.error().GetMessageText().contains("Level1.scene"));
			CHECK(ErrorCodeOf(mount.WriteFileAtomic(Path("project://assets/Other.scene"), AsBytes("{}"))) == ErrorCode::Validation);
		}

		TEST_CASE("MemoryMount: a new name that differs from an entry only in case is a validation error" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.CreateDirectories(Path("project://Assets")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/Level1.scene"), AsBytes("original")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/Other.scene"), AsBytes("other")).has_value());
			const uint64_t mutations = mount.GetMutationCount();

			const Status write = mount.WriteFileAtomic(Path("project://Assets/level1.scene"), AsBytes("replacement"));
			REQUIRE_FALSE(write.has_value());
			CHECK(write.error().GetCode() == ErrorCode::Validation);
			CHECK(write.error().GetMessageText().contains("Level1.scene"));
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://Assets/LEVEL1.scene"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(mount.CreateDirectories(Path("project://ASSETS"))) == ErrorCode::Validation);
			CHECK(ErrorCodeOf(mount.Move(Path("project://Assets/Other.scene"), Path("project://Assets/level1.scene")))
				== ErrorCode::Validation);

			CHECK(mount.GetMutationCount() == mutations);
			const Result<std::vector<VfsEntry>> listed = mount.List(Path("project://Assets"), false);
			REQUIRE(listed.has_value());
			CHECK(listed->size() == 2);
			const Result<Buffer> original = mount.ReadFile(Path("project://Assets/Level1.scene"));
			REQUIRE(original.has_value());
			CHECK(AsStringView(*original) == "original");
		}

		TEST_CASE("MemoryMount: a case-only Move renames the entry in place" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.CreateDirectories(Path("project://Assets")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Assets/level1.scene"), AsBytes("content")).has_value());

			REQUIRE(mount.Move(Path("project://Assets/level1.scene"), Path("project://Assets/Level1.scene")).has_value());
			CHECK(mount.ReadFile(Path("project://Assets/Level1.scene")).has_value());
			CHECK(ErrorCodeOf(mount.ReadFile(Path("project://Assets/level1.scene"))) == ErrorCode::Validation);

			REQUIRE(mount.Move(Path("project://Assets"), Path("project://assets")).has_value());
			CHECK(mount.ReadFile(Path("project://assets/Level1.scene")).has_value());
		}

		TEST_CASE("MemoryMount: List is sorted and recursive listings include directories" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.CreateDirectories(Path("project://b")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://b/c.txt"), AsBytes("c")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://a.txt"), AsBytes("a")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://B.txt"), AsBytes("B")).has_value());

			const Result<std::vector<VfsEntry>> recursive = mount.List(Path("project://"), true);
			REQUIRE(recursive.has_value());
			std::vector<std::string> paths;
			for (const VfsEntry& entry : *recursive)
				paths.push_back(std::string(entry.Path.GetPath()));
			CHECK(paths == std::vector<std::string>{ "B.txt", "a.txt", "b", "b/c.txt" });
			CHECK(ErrorCodeOf(mount.List(Path("project://a.txt"), false)) == ErrorCode::Io);
		}

		TEST_CASE("MemoryMount: Open streams a snapshot taken when it was opened" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.WriteFileAtomic(Path("project://Clip.wav"), AsBytes("original")).has_value());
			Result<Scope<IFileStream>> stream = mount.Open(Path("project://Clip.wav"));
			REQUIRE(stream.has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Clip.wav"), AsBytes("replaced")).has_value());

			std::array<std::byte, 8> bytes{};
			CHECK((*stream)->Read(bytes) == size_t{ 8 });
			CHECK(AsStringView(bytes) == "original");
		}

		TEST_CASE("MemoryMount: Remove deletes directories recursively and refuses the root" * doctest::skip(true))
		{
			MemoryMount mount;
			REQUIRE(mount.CreateDirectories(Path("project://Dir/Sub")).has_value());
			REQUIRE(mount.WriteFileAtomic(Path("project://Dir/Sub/A.txt"), AsBytes("a")).has_value());

			REQUIRE(mount.Remove(Path("project://Dir")).has_value());
			CHECK(ErrorCodeOf(mount.GetInfo(Path("project://Dir/Sub/A.txt"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.Remove(Path("project://Dir"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(mount.Remove(Path("project://"))) == ErrorCode::InvalidArgument);
		}
	}

}
