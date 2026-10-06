#include "TestsPCH.h"

#include "EditorCore/Automation/AutomationServer.h"

#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Platform/Process.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/Utf8Path.h"

namespace Engine {

	static Json ParseServerJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AutomationServer: the launcher state allows only session, rpc, docs and project creation methods" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerLauncher");
			Test::AutomationTestClient client(fixture.GetEditor());
			CHECK(client.Call("session.info", Json::object()).has_value());
			CHECK(client.Call("rpc.discover", Json::object()).has_value());
			CHECK(client.Call("docs.get", Json::object()).has_value());
			for (const std::string_view method : { "scene.tree", "entity.create", "project.info", "component.list", "log.read" })
			{
				INFO(std::string(method));
				const Result<Json> refused = client.Call(method, Json::object());
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
				CHECK(refused.error().GetMessageText().contains("no project open"));
			}
			CHECK(client.Call("project.create", Json{ { "path", Test::PathToUtf8(fixture.GetDirectory() / "Created") }, { "name", "Created" } }).has_value());
			CHECK(client.Call("project.info", Json::object()).has_value());
		}

		TEST_CASE("AutomationServer: read-only editors deny mutations but allow reads and dry runs" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerReadOnly");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			const std::filesystem::path projectFile = fixture.GetEditor().GetProject().GetProjectFile();
			fixture.GetEditor().CloseProject();
			Result<Scope<LoadedProject>> project = ProjectManager::OpenProject(projectFile,
				{ .ReadOnly = true, .StrictUnknowns = false, .ReadOnlyCacheDirectory = fixture.GetDirectory() / "Private" },
				fixture.GetEngine().GetTypeRegistry());
			REQUIRE(project.has_value());
			REQUIRE(fixture.GetEditor().OpenProject(std::move(*project)).has_value());

			Test::AutomationTestClient client(fixture.GetEditor());
			// Opening a scene only changes what the editor shows, so a read-only editor may.
			CHECK(client.Call("scene.open", Json{ { "path", "Assets/Scenes/Main.scene" } }).has_value());
			const Result<Json> denied = client.Call("entity.create", Json{ { "name", "X" } });
			REQUIRE_FALSE(denied.has_value());
			CHECK(denied.error().GetCode() == ErrorCode::PermissionDenied);
			Json response = client.Request("entity.create", Json{ { "name", "X" } });
			CHECK(response["error"]["code"] == Json(-32008));
			CHECK(client.Call("project.info", Json::object()).has_value());
			Result<Json> info = client.Call("session.info", Json::object());
			REQUIRE(info.has_value());
			CHECK((*info)["readOnly"] == Json(true));
		}

		TEST_CASE("AutomationServer: a stale ifRevision is Conflict with the current revision" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerRevision");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			const uint64_t revision = fixture.GetEditor().GetScene().GetRevision();
			CHECK(client.Call("entity.create", Json{ { "name", "A" }, { "ifRevision", revision } }).has_value());
			Json response = client.Request("entity.create", Json{ { "name", "B" }, { "ifRevision", revision } });
			CHECK(response["error"]["code"] == Json(-32004));
			CHECK(response["error"]["data"]["currentRevision"] == Json(fixture.GetEditor().GetScene().GetRevision()));
		}

		TEST_CASE("AutomationServer: an in-process round trip answers with _meta" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerRoundTrip");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			Json response = client.Request("entity.create", Json{ { "name", "Board" } });
			REQUIRE(response.contains("result"));
			Json& meta = response["result"]["_meta"];
			CHECK(meta["revision"] == Json(fixture.GetEditor().GetScene().GetRevision()));
			CHECK(meta["dirty"] == Json(true));
			CHECK(meta["undoLabel"].dump().contains("[agent]"));
			CHECK(meta["playState"] == Json("Edit"));
			CHECK(meta["diagnostics"].contains("logCursor"));
		}

		TEST_CASE("AutomationServer: disconnecting a client cancels its pending operations and appends the event" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerDisconnect");
			Test::AutomationTestClient client(fixture.GetEditor());
			AutomationServer& server = client.GetServer();
			const ClientId other = server.ConnectInProcess("doomed");
			server.SubmitInProcess(other, RpcRequest{ .Id = Json(1), .IsNotification = false, .Method = "debug.pend", .Params = Json{ { "frames", 0 } }, .TranscriptLine = std::nullopt });
			server.Pump();
			CHECK(server.TakeInProcessResponses(other).empty());
			const uint64_t nextEvent = fixture.GetEngine().GetEventLog().GetNextSeq();
			server.DisconnectInProcess(other);
			server.Pump();
			const EventReadResult events = fixture.GetEngine().GetEventLog().Read(nextEvent);
			REQUIRE(events.Events.size() == 1);
			CHECK(events.Events[0].Type == EngineEventType::AutomationClientDisconnected);
			CHECK(events.Events[0].Name == "doomed");
			Result<Json> log = client.Call("log.read", Json{ { "contains", "Cancelled debug.pend" } });
			REQUIRE(log.has_value());
			CHECK((*log)["entries"].size() == 1);
		}

		TEST_CASE("AutomationServer: listening writes a session file that is removed on destruction" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerSession");
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.Listen = true;
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			const std::filesystem::path file = SessionFile::GetPath(specification.SessionsDirectory, Process::GetCurrentId());
			{
				Result<Scope<AutomationServer>> server = AutomationServer::Create(fixture.GetEditor(), specification);
				REQUIRE_MESSAGE(server.has_value(), server.error().ToString());
				CHECK((*server)->GetPort() != 0);
				const Result<std::string> text = FileSystem::ReadText(file);
				REQUIRE(text.has_value());
				const Result<SessionFileContent> content = SessionFile::FromText(*text);
				REQUIRE(content.has_value());
				CHECK(content->Port == (*server)->GetPort());
				CHECK(content->Token.size() == 64);
				CHECK(content->ProjectPath.empty());
				CHECK(content->Headless);

				fixture.CreateAndOpenProject();
				(*server)->Pump();
				const Result<SessionFileContent> rewritten = SessionFile::FromText(FileSystem::ReadText(file).value_or(std::string()));
				REQUIRE(rewritten.has_value());
				CHECK(rewritten->ProjectPath.ends_with("TestProject.eproj"));
			}
			CHECK_FALSE(FileSystem::Exists(file));
		}

		TEST_CASE("AutomationServer: offloaded results land in Library/Automation/Out" * doctest::skip(true))
		{
			Test::EditorTestFixture fixture("ServerOffload");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			Json batch = ParseServerJson(R"({"label":"Many","ops":[]})");
			for (int index = 0; index < 400; ++index)
				batch["ops"].push_back(Json{ { "method", "entity.create" }, { "params", Json{ { "name", std::format("Entity{:04}", index) } } } });
			REQUIRE(client.Call("edit.batch", batch).has_value());
			Result<Json> scene = client.Call("scene.get", Json::object());
			REQUIRE(scene.has_value());
			CHECK((*scene)["truncated"] == Json(true));
			const std::string path = JsonReader((*scene)["path"]).ReadString().value_or(std::string());
			CHECK(path.contains("Library/Automation/Out/"));
			CHECK(FileSystem::Exists(std::filesystem::path(path)));
		}
	}

}
