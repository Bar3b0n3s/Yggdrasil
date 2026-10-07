#include "TestsPCH.h"

#include "Engine/Core/FileSystem.h"

#include "Support/TempDirectory.h"

#include <chrono>
#include <system_error>

namespace Engine {

	// The error code of a failed result; nullopt on success, so a CHECK never reads error() of a value.
	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	static std::string ReadTextOrEmpty(const std::filesystem::path& path)
	{
		Result<std::string> text = FileSystem::ReadText(path);
		return text.has_value() ? std::move(*text) : std::string();
	}

	static std::vector<std::string> FileNames(const std::filesystem::path& directory)
	{
		std::vector<std::string> names;
		Result<std::vector<std::filesystem::path>> entries = FileSystem::ListDirectory(directory);
		if (entries.has_value())
		{
			for (const std::filesystem::path& entry : *entries)
				names.push_back(entry.filename().generic_string());
		}
		return names;
	}

	TEST_SUITE("Core")
	{
		TEST_CASE("FileSystem: atomic write survives an injected failure")
		{
			constexpr std::array<AtomicWriteStep, 5> Steps = {
				AtomicWriteStep::CreateTemporary,
				AtomicWriteStep::Write,
				AtomicWriteStep::Flush,
				AtomicWriteStep::Backup,
				AtomicWriteStep::Replace,
			};

			for (const AtomicWriteStep step : Steps)
			{
				Test::TempDirectory directory("AtomicWrite");
				const std::filesystem::path target = directory / "Level1.scene";
				REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("old content")).has_value());

				AtomicWriteOptions options;
				options.InjectFailure = step;
				const Status status = FileSystem::WriteFileAtomic(target, AsBytes("new content that must not appear"), options);

				INFO("injected step ", static_cast<int>(step));
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::Io);
				CHECK(ReadTextOrEmpty(target) == "old content");
				for (const std::string& name : FileNames(directory.GetPath()))
					CHECK_FALSE(name.contains(".tmp"));
			}
		}

		TEST_CASE("FileSystem: atomic write replaces the content and keeps one backup")
		{
			Test::TempDirectory directory("AtomicWrite");
			const std::filesystem::path target = directory / "Game.eproj";

			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("first")).has_value());
			CHECK(ReadTextOrEmpty(target) == "first");
			CHECK_FALSE(FileSystem::Exists(directory / "Game.eproj.bak"));

			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("second")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("third")).has_value());
			CHECK(ReadTextOrEmpty(target) == "third");
			CHECK(ReadTextOrEmpty(directory / "Game.eproj.bak") == "second");
			CHECK(FileNames(directory.GetPath()) == std::vector<std::string>{ "Game.eproj", "Game.eproj.bak" });

			AtomicWriteOptions noBackup;
			noBackup.KeepBackup = false;
			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("fourth"), noBackup).has_value());
			CHECK(ReadTextOrEmpty(directory / "Game.eproj.bak") == "second");
		}

		TEST_CASE("FileSystem: atomic write into a missing directory is NotFound")
		{
			Test::TempDirectory directory("AtomicWrite");
			const Status status = FileSystem::WriteFileAtomic(directory / "Missing/File.txt", AsBytes("x"));
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: reads return the bytes and validate text as UTF-8")
		{
			Test::TempDirectory directory("Read");
			const std::array<std::byte, 3> invalid = { std::byte{ 'a' }, std::byte{ 0xff }, std::byte{ 'b' } };
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Binary.bin", invalid).has_value());

			const Result<Buffer> bytes = FileSystem::ReadFile(directory / "Binary.bin");
			REQUIRE(bytes.has_value());
			CHECK(*bytes == Buffer(invalid.begin(), invalid.end()));

			const Result<std::string> text = FileSystem::ReadText(directory / "Binary.bin");
			REQUIRE_FALSE(text.has_value());
			CHECK(text.error().GetCode() == ErrorCode::Validation);

			const Result<Buffer> missing = FileSystem::ReadFile(directory / "Missing.bin");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetMessageText().contains("Missing.bin"));
		}

		TEST_CASE("FileSystem: GetInfo reports size, kind and a changing modification time")
		{
			Test::TempDirectory directory("Info");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "A.txt", AsBytes("12345")).has_value());

			const Result<FileInfo> file = FileSystem::GetInfo(directory / "A.txt");
			REQUIRE(file.has_value());
			CHECK(file->Size == 5);
			CHECK_FALSE(file->IsDirectory);

			const Result<FileInfo> folder = FileSystem::GetInfo(directory.GetPath());
			REQUIRE(folder.has_value());
			CHECK(folder->IsDirectory);
			CHECK(folder->Size == 0);

			CHECK(ErrorCodeOf(FileSystem::GetInfo(directory / "B.txt")) == ErrorCode::NotFound);

			// Moving the timestamp a day into the past (instead of sleeping) makes the rewrite below land on a different
			// time at any host timestamp resolution.
			std::error_code error;
			std::filesystem::last_write_time(directory / "A.txt", std::filesystem::file_time_type::clock::now() - std::chrono::hours(24),
				error);
			REQUIRE_FALSE(error);
			const Result<FileInfo> backdated = FileSystem::GetInfo(directory / "A.txt");
			const Result<FileInfo> unchanged = FileSystem::GetInfo(directory / "A.txt");
			REQUIRE(backdated.has_value());
			REQUIRE(unchanged.has_value());
			CHECK(backdated->ModificationTime != file->ModificationTime);
			CHECK(unchanged->ModificationTime == backdated->ModificationTime);

			REQUIRE(FileSystem::WriteFileAtomic(directory / "A.txt", AsBytes("123")).has_value());
			const Result<FileInfo> rewritten = FileSystem::GetInfo(directory / "A.txt");
			REQUIRE(rewritten.has_value());
			CHECK(rewritten->ModificationTime != backdated->ModificationTime);
			CHECK(rewritten->Size == 3);
		}

		TEST_CASE("FileSystem: ListDirectory sorts entries byte-wise and recurses on request")
		{
			Test::TempDirectory directory("List");
			REQUIRE(FileSystem::CreateDirectories(directory / "b/nested").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "a.txt", AsBytes("a")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "B.txt", AsBytes("B")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "b/nested/c.txt", AsBytes("c")).has_value());

			CHECK(FileNames(directory.GetPath()) == std::vector<std::string>{ "B.txt", "a.txt", "b" });

			const Result<std::vector<std::filesystem::path>> recursive = FileSystem::ListDirectory(directory.GetPath(), true);
			REQUIRE(recursive.has_value());
			std::vector<std::string> relative;
			for (const std::filesystem::path& entry : *recursive)
				relative.push_back(entry.lexically_relative(directory.GetPath()).generic_string());
			CHECK(relative == std::vector<std::string>{ "B.txt", "a.txt", "b", "b/nested", "b/nested/c.txt" });
		}

		TEST_CASE("FileSystem: VerifyCase reports a case mismatch on every host")
		{
			Test::TempDirectory directory("Case");
			REQUIRE(FileSystem::CreateDirectories(directory / "Assets/Scenes").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Assets/Scenes/Level1.scene", AsBytes("{}")).has_value());

			CHECK(FileSystem::VerifyCase(directory.GetPath(), "Assets/Scenes/Level1.scene").has_value());

			const Status wrongFile = FileSystem::VerifyCase(directory.GetPath(), "Assets/Scenes/level1.scene");
			REQUIRE_FALSE(wrongFile.has_value());
			CHECK(wrongFile.error().GetCode() == ErrorCode::Validation);
			CHECK(wrongFile.error().GetMessageText().contains("case mismatch"));
			CHECK(wrongFile.error().GetMessageText().contains("Level1.scene"));

			const Status wrongDirectory = FileSystem::VerifyCase(directory.GetPath(), "assets/Scenes/Level1.scene");
			REQUIRE_FALSE(wrongDirectory.has_value());
			CHECK(wrongDirectory.error().GetCode() == ErrorCode::Validation);

			const Status missing = FileSystem::VerifyCase(directory.GetPath(), "Assets/Scenes/Level2.scene");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: Move renames and refuses to overwrite")
		{
			Test::TempDirectory directory("Move");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "From.txt", AsBytes("from")).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Taken.txt", AsBytes("taken")).has_value());

			const Status overwrite = FileSystem::Move(directory / "From.txt", directory / "Taken.txt");
			REQUIRE_FALSE(overwrite.has_value());
			CHECK(overwrite.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(ReadTextOrEmpty(directory / "Taken.txt") == "taken");

			REQUIRE(FileSystem::Move(directory / "From.txt", directory / "To.txt").has_value());
			CHECK(ReadTextOrEmpty(directory / "To.txt") == "from");
			CHECK_FALSE(FileSystem::Exists(directory / "From.txt"));
			CHECK(ErrorCodeOf(FileSystem::Move(directory / "From.txt", directory / "Again.txt")) == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: a case-only Move renames a file and a directory in place")
		{
			Test::TempDirectory directory("MoveCase");
			REQUIRE(FileSystem::CreateDirectories(directory / "assets").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "assets/level1.scene", AsBytes("content")).has_value());

			REQUIRE(FileSystem::Move(directory / "assets/level1.scene", directory / "assets/Level1.scene").has_value());
			REQUIRE(FileSystem::Move(directory / "assets", directory / "Assets").has_value());

			CHECK(FileNames(directory.GetPath()) == std::vector<std::string>{ "Assets" });
			CHECK(FileNames(directory / "Assets") == std::vector<std::string>{ "Level1.scene" });
			CHECK(ReadTextOrEmpty(directory / "Assets/Level1.scene") == "content");
		}

		TEST_CASE("FileSystem: Move into its own subtree is InvalidArgument")
		{
			Test::TempDirectory directory("MoveInto");
			REQUIRE(FileSystem::CreateDirectories(directory / "A/B").has_value());
			CHECK(ErrorCodeOf(FileSystem::Move(directory / "A", directory / "A/B/C")) == ErrorCode::InvalidArgument);
			CHECK(FileSystem::Exists(directory / "A/B"));
		}

		TEST_CASE("FileSystem: writes into a directory, reads of a directory and listings of a file are Io errors")
		{
			Test::TempDirectory directory("Kinds");
			REQUIRE(FileSystem::CreateDirectories(directory / "Folder").has_value());
			REQUIRE(FileSystem::WriteFileAtomic(directory / "File.txt", AsBytes("x")).has_value());

			CHECK(ErrorCodeOf(FileSystem::WriteFileAtomic(directory / "Folder", AsBytes("x"))) == ErrorCode::Io);
			CHECK(ErrorCodeOf(FileSystem::ReadFile(directory / "Folder")) == ErrorCode::Io);
			CHECK(ErrorCodeOf(FileSystem::ListDirectory(directory / "File.txt")) == ErrorCode::Io);
			CHECK(ErrorCodeOf(FileSystem::ListDirectory(directory / "Missing")) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(FileSystem::Remove(directory / "Missing")) == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: ReadText returns the bytes unchanged")
		{
			Test::TempDirectory directory("Text");
			const std::string text = std::string("\xef\xbb\xbf") + "line one\r\nline two\n";
			REQUIRE(FileSystem::WriteFileAtomic(directory / "Text.txt", AsBytes(text)).has_value());
			CHECK(ReadTextOrEmpty(directory / "Text.txt") == text);

			REQUIRE(FileSystem::WriteFileAtomic(directory / "Empty.txt", {}).has_value());
			const Result<Buffer> empty = FileSystem::ReadFile(directory / "Empty.txt");
			REQUIRE(empty.has_value());
			CHECK(empty->empty());
		}

		TEST_CASE("FileSystem: VerifyCase of a missing root is NotFound")
		{
			Test::TempDirectory directory("CaseRoot");
			CHECK(ErrorCodeOf(FileSystem::VerifyCase(directory / "Missing", "A.txt")) == ErrorCode::NotFound);
			CHECK(FileSystem::VerifyCase(directory.GetPath(), "").has_value());

			REQUIRE(FileSystem::WriteFileAtomic(directory / "File.txt", AsBytes("x")).has_value());
			CHECK(ErrorCodeOf(FileSystem::VerifyCase(directory.GetPath(), "File.txt/Below.txt")) == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: CreateDirectories succeeds when the directory exists and Remove is recursive")
		{
			Test::TempDirectory directory("Directories");
			REQUIRE(FileSystem::CreateDirectories(directory / "A/B/C").has_value());
			CHECK(FileSystem::CreateDirectories(directory / "A/B/C").has_value());

			REQUIRE(FileSystem::WriteFileAtomic(directory / "A/B/C/File.txt", AsBytes("x")).has_value());
			const Status blocked = FileSystem::CreateDirectories(directory / "A/B/C/File.txt");
			REQUIRE_FALSE(blocked.has_value());
			CHECK(blocked.error().GetCode() == ErrorCode::AlreadyExists);

			REQUIRE(FileSystem::Remove(directory / "A").has_value());
			CHECK_FALSE(FileSystem::Exists(directory / "A"));
			CHECK(ErrorCodeOf(FileSystem::Remove(directory / "A")) == ErrorCode::NotFound);
		}

		TEST_CASE("FileSystem: a path below a file does not exist, and CreateDirectories reports the file in the way")
		{
			// POSIX hosts report such a path with ENOTDIR and Windows as not found; both mean that nothing is there.
			Test::TempDirectory directory("BelowFile");
			REQUIRE(FileSystem::WriteFileAtomic(directory / "File.txt", AsBytes("x")).has_value());
			const std::filesystem::path below = directory / "File.txt/Nested/Leaf.txt";

			CHECK(ErrorCodeOf(FileSystem::ReadFile(below)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(FileSystem::GetInfo(below)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(FileSystem::Remove(below)) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(FileSystem::WriteFileAtomic(below, AsBytes("y"))) == ErrorCode::NotFound);
			CHECK(ErrorCodeOf(FileSystem::ListDirectory(below)) == ErrorCode::NotFound);
			CHECK_FALSE(FileSystem::Exists(below));

			const Status created = FileSystem::CreateDirectories(directory / "File.txt/Nested");
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::AlreadyExists);
			CHECK(created.error().GetMessageText().contains("File.txt"));
			CHECK(FileSystem::ReadText(directory / "File.txt") == std::string("x"));
		}

		TEST_CASE("FileSystem: PathToUtf8 and PathFromUtf8 convert UTF-8 text and never throw on names the host allows")
		{
			const std::string text = "Projects/\xC3\xA9t\xC3\xA9/Main.scene"; // "Projects/ete/Main.scene" with two e-acute
			const std::filesystem::path path = FileSystem::PathFromUtf8(text);
			CHECK(path.filename() == std::filesystem::path(u8"Main.scene"));
			CHECK(path.parent_path().filename() == std::filesystem::path(u8"été"));
			CHECK(FileSystem::PathToUtf8(path) == text);
			CHECK(FileSystem::PathToUtf8(FileSystem::PathFromUtf8("")).empty());
			CHECK(FileSystem::PathToUtf8(FileSystem::PathFromUtf8("a\xff")) == "a\xEF\xBF\xBD"); // U+FFFD for the ill-formed byte

			// An unpaired UTF-16 surrogate on Windows (path::generic_u8string throws for it), an invalid UTF-8 byte elsewhere
			// (returned unchanged).
			using NativeChar = std::filesystem::path::value_type;
			std::basic_string<NativeChar> name;
			name += static_cast<NativeChar>('B');
			name += static_cast<NativeChar>(sizeof(NativeChar) == 1 ? 0xff : 0xd800);
			const std::string converted = FileSystem::PathToUtf8(std::filesystem::path(name));
			CHECK(converted == (sizeof(NativeChar) == 1 ? std::string("B\xff") : std::string("B\xEF\xBF\xBD")));
		}
	}

}
