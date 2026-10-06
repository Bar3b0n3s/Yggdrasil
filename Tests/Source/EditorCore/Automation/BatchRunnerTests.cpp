#include "TestsPCH.h"

#include "EditorCore/Automation/BatchRunner.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	static std::filesystem::path WriteBatchFile(const Test::EditorTestFixture& fixture, std::string_view text)
	{
		const std::filesystem::path file = fixture.GetDirectory() / "Scaffold.jsonl";
		REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))).has_value());
		return file;
	}

	// Advances the runner frame by frame (as EditorApp does) until it finishes, at most `frames` frames.
	static void RunBatch(BatchRunner& runner, AutomationServer& server, int frames = 1000)
	{
		for (int frame = 0; frame < frames && !runner.IsFinished(); ++frame)
		{
			server.Pump();
			runner.Advance(server);
		}
		REQUIRE(runner.IsFinished());
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("BatchRunner: a batch file runs line by line with line references" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("BatchRun");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			const std::filesystem::path file = WriteBatchFile(fixture,
				"{\"method\":\"entity.create\",\"params\":{\"name\":\"Game\"}}\n"
				"{\"method\":\"entity.create\",\"params\":{\"name\":\"Board\",\"parent\":{\"$ref\":\"1.entity.id\"}}}\r\n");
			Result<std::vector<BatchRequest>> requests = BatchRunner::LoadFile(file);
			REQUIRE_MESSAGE(requests.has_value(), requests.error().ToString());
			REQUIRE(requests->size() == 2);
			CHECK((*requests)[1].Line == 2);

			BatchRunner runner(std::move(*requests), {});
			RunBatch(runner, client.GetServer());
			CHECK_FALSE(runner.GetFailure().has_value());
			CHECK(runner.GetResults().size() == 2);
			CHECK(fixture.GetEditor().GetScene().FindEntityByPath("/Game/Board").IsValid());
		}

		TEST_CASE("BatchRunner: a $ref into a result over the offload threshold reads the full result" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("BatchLargeRef");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());

			// Line 1: an edit.batch of 600 creations, whose result is far over 48 KB; line 2 refers into its last op.
			Json ops = Json::array();
			for (int index = 0; index < 600; ++index)
				ops.push_back(Json{ { "method", "entity.create" }, { "params", Json{ { "name", std::format("Cell{}", index) } } } });
			const std::string first = Json{ { "method", "edit.batch" }, { "params", Json{ { "label", "Grid" }, { "ops", ops } } } }.dump();
			const std::string second = R"({"method":"entity.create","params":{"name":"Marker","parent":{"$ref":"1.results.599.entity.id"}}})";
			const std::filesystem::path file = WriteBatchFile(fixture, first + "\n" + second + "\n");
			Result<std::vector<BatchRequest>> requests = BatchRunner::LoadFile(file);
			REQUIRE_MESSAGE(requests.has_value(), requests.error().ToString());

			BatchRunner runner(std::move(*requests), {});
			RunBatch(runner, client.GetServer());
			CHECK_FALSE(runner.GetFailure().has_value());
			REQUIRE(runner.GetResults().size() == 2);
			CHECK_FALSE(runner.GetResults()[0].contains("truncated"));
			CHECK(fixture.GetEditor().GetScene().FindEntityByPath("/Cell599/Marker").IsValid());
		}

		TEST_CASE("BatchRunner: the first error stops the run and names the line" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("BatchFailure");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			const std::filesystem::path file = WriteBatchFile(fixture,
				"{\"method\":\"entity.create\",\"params\":{\"name\":\"A\"}}\n"
				"{\"method\":\"entity.get\",\"params\":{\"entity\":\"/Missing\"}}\n"
				"{\"method\":\"entity.create\",\"params\":{\"name\":\"C\"}}\n");
			Result<std::vector<BatchRequest>> requests = BatchRunner::LoadFile(file);
			REQUIRE(requests.has_value());
			BatchRunner runner(std::move(*requests), {});
			RunBatch(runner, client.GetServer());
			REQUIRE(runner.GetFailure().has_value());
			CHECK(runner.GetFailure()->GetCode() == ErrorCode::NotFound);
			CHECK(runner.GetFailure()->ToString().contains("line 2"));
			CHECK(runner.GetResults().size() == 1);
			CHECK_FALSE(fixture.GetEditor().GetScene().FindEntityByPath("/C").IsValid());
		}

		TEST_CASE("BatchRunner: malformed lines are located load errors" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("BatchMalformed");
			const std::array<std::pair<std::string_view, ErrorCode>, 4> cases = { {
				{ "{\"method\":\"session.info\"}\n{\"method\":\n", ErrorCode::Parse },
				{ "{\"method\":\"session.info\"}\n\n{\"method\":\"session.info\"}\n", ErrorCode::Validation },
				{ "{\"params\":{}}\n", ErrorCode::Validation },
				{ "{\"method\":\"session.info\",\"extra\":1}\n", ErrorCode::Validation },
			} };
			for (const auto& [text, code] : cases)
			{
				INFO(std::string(text));
				const Result<std::vector<BatchRequest>> requests = BatchRunner::LoadFile(WriteBatchFile(fixture, text));
				REQUIRE_FALSE(requests.has_value());
				CHECK(requests.error().GetCode() == code);
				CHECK(requests.error().GetLocation().Line != 0);
			}
			CHECK(BatchRunner::LoadFile(fixture.GetDirectory() / "Missing.jsonl").error().GetCode() == ErrorCode::NotFound);
		}

		TEST_CASE("BatchRunner: transcript mode appends request and response lines and passes the line number" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("BatchTranscript");
			fixture.CreateAndOpenProject();
			Test::AutomationTestClient client(fixture.GetEditor());
			std::vector<BatchRequest> requests = { BatchRequest{ .Method = "project.upgrade", .Params = {}, .Line = 0 } };
			BatchRunner runner(std::move(requests), BatchRunOptions{ .ClientName = "cli", .WriteTranscript = true });
			RunBatch(runner, client.GetServer());
			CHECK_FALSE(runner.GetFailure().has_value());
			const Result<std::string> transcript = FileSystem::ReadText(fixture.GetProjectRoot() / "Automation/BuildLog.jsonl");
			REQUIRE(transcript.has_value());
			CHECK(std::count(transcript->begin(), transcript->end(), '\n') == 2);
			CHECK((transcript->contains("\"method\": \"project.upgrade\"") || transcript->contains("\"method\":\"project.upgrade\"")));
			CHECK((transcript->contains("\"client\": \"cli\"") || transcript->contains("\"client\":\"cli\"")));
		}
	}

}
