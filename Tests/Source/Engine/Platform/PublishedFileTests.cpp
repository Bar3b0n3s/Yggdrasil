#include "TestsPCH.h"
#include "Engine/Platform/PublishedFile.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <system_error>

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("PublishedFile: read returns every byte and reports a missing file or a directory")
		{
			Test::TempDirectory directory("PublishedFileRead");
			std::string text(200 * 1024 + 7, 'x');
			for (size_t index = 0; index < text.size(); index += 4099)
				text[index] = static_cast<char>('a' + index % 26);
			const std::filesystem::path file = directory / "Large.json";
			REQUIRE(FileSystem::WriteFileAtomic(file, AsBytes(text), { .KeepBackup = false }).has_value());
			const Result<Buffer> read = PublishedFile::Read(file);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(AsStringView(*read) == text);

			const std::filesystem::path empty = directory / "Empty.json";
			REQUIRE(FileSystem::WriteFileAtomic(empty, {}, { .KeepBackup = false }).has_value());
			const Result<Buffer> nothing = PublishedFile::Read(empty);
			REQUIRE_MESSAGE(nothing.has_value(), nothing.error().ToString());
			CHECK(nothing->empty());

			const Result<Buffer> missing = PublishedFile::Read(directory / "Missing.json");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			const Result<Buffer> folder = PublishedFile::Read(directory.GetPath());
			REQUIRE_FALSE(folder.has_value());
			CHECK(folder.error().GetCode() == ErrorCode::Io);
		}

		TEST_CASE("PublishedFile: replace creates or replaces the destination and reports a missing source")
		{
			Test::TempDirectory directory("PublishedFileReplace");
			const std::filesystem::path target = directory / "1.json";
			const std::filesystem::path first = directory / "1.json.first";
			const std::filesystem::path second = directory / "1.json.second";
			REQUIRE(FileSystem::WriteFileAtomic(first, AsBytes("first"), { .KeepBackup = false }).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(second, AsBytes("second"), { .KeepBackup = false }).has_value());

			const Status created = PublishedFile::Replace(first, target);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			CHECK_FALSE(FileSystem::Exists(first));
			CHECK(FileSystem::ReadText(target).value_or("") == "first");

			const Status replaced = PublishedFile::Replace(second, target);
			REQUIRE_MESSAGE(replaced.has_value(), replaced.error().ToString());
			CHECK_FALSE(FileSystem::Exists(second));
			CHECK(FileSystem::ReadText(target).value_or("") == "second");

			const Status missing = PublishedFile::Replace(first, target);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(FileSystem::ReadText(target).value_or("") == "second");
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		// Docs/Decisions/0040-session-file-sharing.md: on Windows a rename keeps a handle with DELETE access open on the
		// file under its new name until the rename returns; a reader that shares deletion opens it in that moment.
		TEST_CASE("PublishedFile: read opens a file while the rename that put it there is still in flight")
		{
			Test::TempDirectory directory("PublishedFileRenameInFlight");
			const std::filesystem::path staged = directory / "1.json.tmp";
			const std::filesystem::path target = directory / "1.json";
			REQUIRE(FileSystem::WriteFileAtomic(staged, AsBytes("{}"), { .KeepBackup = false }).has_value());
			const Result<Test::HeldFileHandle> renaming = Test::RenameKeepingHandleOpen(staged, target);
			REQUIRE_MESSAGE(renaming.has_value(), renaming.error().ToString());
			const Result<Buffer> read = PublishedFile::Read(target);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(AsStringView(*read) == "{}");
		}

		// A reader holding the destination open with Read's sharing keeps the content it opened; later opens find the new one.
		TEST_CASE("PublishedFile: replace succeeds while a reader that shares deletion holds the destination open")
		{
			Test::TempDirectory directory("PublishedFileReplaceWhileRead");
			const std::filesystem::path target = directory / "1.json";
			const std::filesystem::path staged = directory / "1.json.tmp";
			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("old"), { .KeepBackup = false }).has_value());
			REQUIRE(FileSystem::WriteFileAtomic(staged, AsBytes("new"), { .KeepBackup = false }).has_value());
			const Result<Test::HeldFileHandle> reading = Test::OpenForReadingSharingAll(target);
			REQUIRE_MESSAGE(reading.has_value(), reading.error().ToString());

			const Status replaced = PublishedFile::Replace(staged, target);
			REQUIRE_MESSAGE(replaced.has_value(), replaced.error().ToString());
			CHECK(Test::ReadRemaining(*reading).value_or("") == "old");
			const Result<Buffer> read = PublishedFile::Read(target);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(AsStringView(*read) == "new");
		}
#endif

		TEST_CASE("PublishedFile: a durable write replaces the file and leaves no temporary")
		{
			const Test::TempDirectory temp;
			const std::filesystem::path file = temp.GetPath() / "Store.json";
			const std::string first = "first";
			const std::string second = "second, longer";
			const Status written = PublishedFile::WriteDurably(file, std::as_bytes(std::span(first.data(), first.size())));
#if defined(ENGINE_PLATFORM_WINDOWS)
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			REQUIRE(PublishedFile::WriteDurably(file, std::as_bytes(std::span(second.data(), second.size()))).has_value());
			CHECK(FileSystem::ReadText(file).value_or("") == second);
			std::error_code error;
			size_t entries = 0;
			for (std::filesystem::directory_iterator entry(temp.GetPath(), error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
				++entries;
			CHECK_FALSE(error);
			CHECK(entries == 1);
			// Its parent must exist.
			CHECK_FALSE(PublishedFile::WriteDurably(temp.GetPath() / "Missing" / "Store.json", std::as_bytes(std::span(first.data(), first.size()))).has_value());
			// The W28.2 review's finding 8b: a rename that fails (a directory holds the name) removes the temporary it wrote.
			const std::filesystem::path blocked = temp.GetPath() / "Blocked";
			REQUIRE(FileSystem::CreateDirectories(blocked / "Inside").has_value());
			CHECK_FALSE(PublishedFile::WriteDurably(blocked, std::as_bytes(std::span(first.data(), first.size()))).has_value());
			CHECK(std::filesystem::is_directory(blocked / "Inside", error));
			size_t afterFailure = 0;
			for (std::filesystem::directory_iterator entry(temp.GetPath(), error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
				++afterFailure;
			CHECK_FALSE(error);
			CHECK(afterFailure == 2); // Store.json and Blocked, no temporary
#else
			REQUIRE_FALSE(written.has_value());
			CHECK(written.error().GetCode() == ErrorCode::Unsupported);
#endif
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		// A reader sharing deletion holds the store open (another process scanning it): the write-through rename cannot
		// replace it, so the write falls back to Replace's POSIX rename; the reader keeps the content it opened.
		TEST_CASE("PublishedFile: a durable write replaces a file that a reader sharing deletion holds open")
		{
			Test::TempDirectory directory("PublishedFileDurableWhileRead");
			const std::filesystem::path target = directory / "Store.json";
			REQUIRE(FileSystem::WriteFileAtomic(target, AsBytes("old"), { .KeepBackup = false }).has_value());
			const Result<Test::HeldFileHandle> reading = Test::OpenForReadingSharingAll(target);
			REQUIRE_MESSAGE(reading.has_value(), reading.error().ToString());

			const Status written = PublishedFile::WriteDurably(target, AsBytes("new"));
			REQUIRE_MESSAGE(written.has_value(), written.error().ToString());
			CHECK(Test::ReadRemaining(*reading).value_or("") == "old");
			CHECK(FileSystem::ReadText(target).value_or("") == "new");
		}
#endif
	}

}
