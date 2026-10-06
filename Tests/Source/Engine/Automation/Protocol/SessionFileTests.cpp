#include "TestsPCH.h"

#include "Engine/Automation/Protocol/SessionFile.h"

#include "Engine/Core/FileSystem.h"
#include "Support/TempDirectory.h"

namespace Engine {

	static SessionFileContent MakeSessionContent()
	{
		return SessionFileContent{
			.Pid = 4321,
			.Port = 50123,
			.Token = std::string(64, 'f'),
			.ProtocolVersionText = "1.0",
			.EngineVersionText = "0.1.0",
			.ProjectPath = "C:/Projects/Tetris/Tetris.eproj",
			.Headless = true,
			.StartedAt = "2026-10-06T12:34:56Z",
		};
	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("SessionFile: content round-trips through its text with camelCase keys in order" * doctest::skip(true))
		{
			const SessionFileContent content = MakeSessionContent();
			const std::string text = SessionFile::ToText(content);
			const std::array<std::string_view, 8> keys = { "\"pid\"", "\"port\"", "\"token\"", "\"protocolVersion\"", "\"engineVersion\"",
				"\"projectPath\"", "\"headless\"", "\"startedAt\"" };
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
			CHECK(read->ProjectPath == content.ProjectPath);
			CHECK(read->Headless);
			CHECK(read->StartedAt == content.StartedAt);
		}

		TEST_CASE("SessionFile: malformed files name the member and unknown members are ignored" * doctest::skip(true))
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

		TEST_CASE("SessionFile: write creates the directory and remove deletes the file" * doctest::skip(true))
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

		TEST_CASE("SessionFile: timestamps are UTC ISO 8601 with seconds" * doctest::skip(true))
		{
			CHECK(SessionFile::FormatUtcTimestamp(std::chrono::system_clock::time_point{}) == "1970-01-01T00:00:00Z");
			const std::chrono::system_clock::time_point leapDay{ std::chrono::seconds(951782400 + 3723) }; // 2000-02-29 01:02:03
			CHECK(SessionFile::FormatUtcTimestamp(leapDay) == "2000-02-29T01:02:03Z");
		}
	}

}
