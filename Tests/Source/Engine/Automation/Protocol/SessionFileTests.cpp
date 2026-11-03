#include "TestsPCH.h"

#include "Engine/Automation/Protocol/SessionFile.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/BuildId.h"
#include "Engine/Core/FileSystem.h"
#include "Support/PlatformProbes.h"
#include "Support/TempDirectory.h"

#include <format>
#include <string>
#include <string_view>

namespace Engine {

	static SessionFileContent MakeSessionContent()
	{
		return SessionFileContent{
			.Pid = 4321,
			.Port = 50123,
			.Token = std::string(64, 'f'),
			.ProtocolVersionText = "1.0",
			.EngineVersionText = "0.1.0",
			.BuildText = "0.1.0+3f2a9c1d4e5b.dirty",
			.ProjectPath = "C:/Projects/Tetris/Tetris.eproj",
			.Headless = true,
			.StartedAt = "2026-10-06T12:34:56Z",
		};
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("SessionFile: content round-trips through its text with camelCase keys in order")
		{
			const SessionFileContent content = MakeSessionContent();
			const std::string text = SessionFile::ToText(content);
			const std::array<std::string_view, 9> keys = { "\"pid\"", "\"port\"", "\"token\"", "\"protocolVersion\"", "\"engineVersion\"",
				"\"build\"", "\"projectPath\"", "\"headless\"", "\"startedAt\"" };
			size_t previous = 0;
			for (const std::string_view key : keys)
			{
				const size_t position = text.find(key);
				INFO(std::string(key));
				REQUIRE(position != std::string::npos);
				CHECK(position >= previous);
				previous = position;
			}

			const Result<SessionFileContent> read = SessionFile::FromText(text);
			REQUIRE(read.has_value());
			CHECK(read->Pid == content.Pid);
			CHECK(read->Port == content.Port);
			CHECK(read->Token == content.Token);
			CHECK(read->ProtocolVersionText == "1.0");
			CHECK(read->EngineVersionText == "0.1.0");
			CHECK(read->BuildText == "0.1.0+3f2a9c1d4e5b.dirty");
			CHECK(read->ProjectPath == content.ProjectPath);
			CHECK(read->Headless);
			CHECK(read->StartedAt == content.StartedAt);
		}

		// Docs/Decisions/0030-m19-contract.md decision 13 (B-BVER): a server's session file names its build, which a content
		// the server leaves unset takes from this binary; a file an engine before M19 wrote has none and reads with an empty
		// one, and a build that is not a string names the member.
		TEST_CASE("SessionFile: the build id is this binary's unless set, and a file without one reads with an empty one")
		{
			CHECK(SessionFileContent{}.BuildText == FormatBuildId(GetBuildId()));
			CHECK(SessionFile::ToText(SessionFileContent{}).contains(std::format("\"build\": \"{}\"", FormatBuildId(GetBuildId()))));

			std::string earlier = SessionFile::ToText(MakeSessionContent());
			const size_t begin = earlier.find("\t\"build\"");
			REQUIRE(begin != std::string::npos);
			earlier.erase(begin, earlier.find('\n', begin) + 1 - begin);
			const Result<SessionFileContent> read = SessionFile::FromText(earlier);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->BuildText.empty());
			CHECK(read->EngineVersionText == "0.1.0");

			std::string numbered = SessionFile::ToText(MakeSessionContent());
			numbered.replace(numbered.find("\"0.1.0+3f2a9c1d4e5b.dirty\""), std::string_view("\"0.1.0+3f2a9c1d4e5b.dirty\"").size(), "7");
			const Result<SessionFileContent> refused = SessionFile::FromText(numbered);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(refused.error().ToString().contains("build"));
		}

		TEST_CASE("SessionFile: malformed files name the member and unknown members are ignored")
		{
			CHECK(SessionFile::FromText("{").error().GetCode() == ErrorCode::Parse);
			const Result<SessionFileContent> missing = SessionFile::FromText(R"({"pid": 1})");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::Validation);
			CHECK(missing.error().ToString().contains("port"));

			std::string extended = SessionFile::ToText(MakeSessionContent());
			extended.insert(1, "\"future\": 1, ");
			CHECK(SessionFile::FromText(extended).has_value());
		}

		TEST_CASE("SessionFile: write creates the directory and remove deletes the file")
		{
			Test::TempDirectory directory("SessionFile");
			const std::filesystem::path sessions = directory / "Automation/Sessions";
			const SessionFileContent content = MakeSessionContent();
			REQUIRE(SessionFile::Write(sessions, content).has_value());

			const std::filesystem::path file = SessionFile::GetPath(sessions, content.Pid);
			CHECK(file == sessions / "4321.json");
			const Result<std::string> text = FileSystem::ReadText(file);
			REQUIRE(text.has_value());
			CHECK(*text == SessionFile::ToText(content));
#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)
			// The token is the server's only secret: user-only permissions (§13.2).
			std::error_code error;
			const std::filesystem::perms filePermissions = std::filesystem::status(file, error).permissions();
			CHECK((filePermissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
			const std::filesystem::perms directoryPermissions = std::filesystem::status(sessions, error).permissions();
			CHECK((directoryPermissions & (std::filesystem::perms::group_all | std::filesystem::perms::others_all)) == std::filesystem::perms::none);
#endif

			REQUIRE(SessionFile::Remove(sessions, content.Pid).has_value());
			CHECK_FALSE(FileSystem::Exists(file));
			CHECK(SessionFile::Remove(sessions, content.Pid).has_value());
		}

		TEST_CASE("SessionFile: read returns the written content and reports a missing file")
		{
			Test::TempDirectory directory("SessionFileRead");
			const std::filesystem::path sessions = directory / "Sessions";
			const SessionFileContent content = MakeSessionContent();
			const Result<SessionFileContent> missing = SessionFile::Read(sessions, content.Pid);
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);

			REQUIRE(SessionFile::Write(sessions, content).has_value());
			const Result<SessionFileContent> read = SessionFile::Read(sessions, content.Pid);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Pid == content.Pid);
			CHECK(read->Port == content.Port);
			CHECK(read->Token == content.Token);
			CHECK(read->ProjectPath == content.ProjectPath);
		}

#if defined(ENGINE_PLATFORM_WINDOWS)
		// Docs/Decisions/0040-session-file-sharing.md: a client that polls for a server's session file can open it in the
		// moment after the rename that published it took effect and before the rename returned, while the rename's handle
		// with DELETE access is still open on it. A reader that does not share deletion failed there with PermissionDenied
		// (the GPU case "RuntimeApp: a game-view render that runs out of memory ..." did once on a loaded machine).
		TEST_CASE("SessionFile: a client reads the file while the rename that publishes it is still in flight")
		{
			Test::TempDirectory directory("SessionFileRenameInFlight");
			const std::filesystem::path sessions = directory / "Sessions";
			REQUIRE(FileSystem::CreateDirectories(sessions).has_value());
			const SessionFileContent content = MakeSessionContent();
			const std::filesystem::path file = SessionFile::GetPath(sessions, content.Pid);
			std::filesystem::path staged = file;
			staged += ".tmp";
			REQUIRE(FileSystem::WriteFileAtomic(staged, AsBytes(SessionFile::ToText(content)), { .KeepBackup = false }).has_value());

			const Result<Test::HeldFileHandle> renaming = Test::RenameKeepingHandleOpen(staged, file);
			REQUIRE_MESSAGE(renaming.has_value(), renaming.error().ToString());
			REQUIRE(FileSystem::Exists(file));
			const Result<SessionFileContent> read = SessionFile::Read(sessions, content.Pid);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Port == content.Port);
			CHECK(read->Token == content.Token);
		}

		// Docs/Decisions/0040-session-file-sharing.md: the editor rewrites its session file when a project opens or closes,
		// and a client may be reading it then. MoveFileExW cannot replace a file that any reader has open, so the rewrite
		// failed; the POSIX-semantics rename replaces it, the client keeps the content it opened, and the next read finds
		// the rewrite. Removing the file at exit succeeds while a client reads it too.
		TEST_CASE("SessionFile: rewriting and removing the file succeed while a client is reading it")
		{
			Test::TempDirectory directory("SessionFileRewriteWhileRead");
			const std::filesystem::path sessions = directory / "Sessions";
			const SessionFileContent content = MakeSessionContent();
			REQUIRE(SessionFile::Write(sessions, content).has_value());
			const std::filesystem::path file = SessionFile::GetPath(sessions, content.Pid);

			const Result<Test::HeldFileHandle> reading = Test::OpenForReadingSharingAll(file);
			REQUIRE_MESSAGE(reading.has_value(), reading.error().ToString());
			SessionFileContent reopened = content;
			reopened.ProjectPath = "C:/Projects/Breakout/Breakout.eproj";
			const Status rewritten = SessionFile::Write(sessions, reopened);
			REQUIRE_MESSAGE(rewritten.has_value(), rewritten.error().ToString());
			const Result<std::string> opened = Test::ReadRemaining(*reading);
			REQUIRE_MESSAGE(opened.has_value(), opened.error().ToString());
			CHECK(*opened == SessionFile::ToText(content));
			const Result<SessionFileContent> read = SessionFile::Read(sessions, content.Pid);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->ProjectPath == reopened.ProjectPath);
			std::filesystem::path staged = file;
			staged += ".tmp";
			CHECK_FALSE(FileSystem::Exists(staged));

			const Result<Test::HeldFileHandle> stillReading = Test::OpenForReadingSharingAll(file);
			REQUIRE_MESSAGE(stillReading.has_value(), stillReading.error().ToString());
			const Status removed = SessionFile::Remove(sessions, content.Pid);
			REQUIRE_MESSAGE(removed.has_value(), removed.error().ToString());
			CHECK_FALSE(FileSystem::Exists(file));
		}
#endif

		TEST_CASE("SessionFile: timestamps are UTC ISO 8601 with seconds")
		{
			CHECK(SessionFile::FormatUtcTimestamp(std::chrono::system_clock::time_point{}) == "1970-01-01T00:00:00Z");
			const std::chrono::system_clock::time_point leapDay{ std::chrono::seconds(951782400 + 3723) }; // 2000-02-29 01:02:03
			CHECK(SessionFile::FormatUtcTimestamp(leapDay) == "2000-02-29T01:02:03Z");
		}

		TEST_CASE("SessionFile: timestamps with milliseconds round down, also before the epoch")
		{
			const std::chrono::system_clock::time_point leapDay{ std::chrono::milliseconds(951782400000LL + 3723512) }; // 2000-02-29 01:02:03.512
			CHECK(SessionFile::FormatUtcTimestamp(leapDay, TimestampPrecision::Milliseconds) == "2000-02-29T01:02:03.512Z");
			CHECK(SessionFile::FormatUtcTimestamp(leapDay) == "2000-02-29T01:02:03Z");
			const std::chrono::system_clock::time_point beforeEpoch{ std::chrono::milliseconds(-1) };
			CHECK(SessionFile::FormatUtcTimestamp(beforeEpoch, TimestampPrecision::Milliseconds) == "1969-12-31T23:59:59.999Z");
			CHECK(SessionFile::FormatUtcTimestamp(beforeEpoch) == "1969-12-31T23:59:59Z");
		}
	}

}
