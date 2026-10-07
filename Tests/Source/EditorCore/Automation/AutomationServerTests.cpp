#include "TestsPCH.h"

#include "EditorCore/Automation/AutomationServer.h"

#include "Engine/Automation/Protocol/Framing.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Socket.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/Utf8Path.h"
#include "Support/WaitUntil.h"

namespace Engine {

	static Json ParseServerJson(std::string_view text)
	{
		Result<Json> json = JsonReader::Parse(text);
		REQUIRE(json.has_value());
		return std::move(*json);
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("AutomationServer: the launcher state allows only session, rpc, docs and project creation methods")
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

		TEST_CASE("AutomationServer: read-only editors deny mutations but allow reads and dry runs")
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

		TEST_CASE("AutomationServer: a stale ifRevision is Conflict with the current revision")
		{
			Test::EditorTestFixture fixture("ServerRevision");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			// ifRevision compares with the editor's revision, which never repeats across scenes (ADR 0008 decision 28).
			const uint64_t revision = fixture.GetEditor().GetRevision();
			CHECK(client.Call("entity.create", Json{ { "name", "A" }, { "ifRevision", revision } }).has_value());
			Json response = client.Request("entity.create", Json{ { "name", "B" }, { "ifRevision", revision } });
			CHECK(response["error"]["code"] == Json(-32004));
			CHECK(response["error"]["data"]["currentRevision"] == Json(fixture.GetEditor().GetRevision()));

			// A call that changes something only when asked (a validator fix) is guarded the same way, and a read takes the
			// current revision as a precondition.
			Json fix = client.Request("project.validate", Json{ { "fix", true }, { "ifRevision", revision } });
			CHECK(fix["error"]["code"] == Json(-32004));
			CHECK(fix["error"]["data"]["currentRevision"] == Json(fixture.GetEditor().GetRevision()));
			CHECK(client.Call("project.validate", Json{ { "fix", true }, { "ifRevision", fixture.GetEditor().GetRevision() } }).has_value());
			CHECK(client.Call("scene.tree", Json{ { "ifRevision", fixture.GetEditor().GetRevision() } }).has_value());
			CHECK(client.Request("scene.tree", Json{ { "ifRevision", revision } })["error"]["code"] == Json(-32004));
		}

		TEST_CASE("AutomationServer: an in-process round trip answers with _meta")
		{
			Test::EditorTestFixture fixture("ServerRoundTrip");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Test::AutomationTestClient client(fixture.GetEditor());
			Json response = client.Request("entity.create", Json{ { "name", "Board" } });
			REQUIRE(response.contains("result"));
			Json& meta = response["result"]["_meta"];
			CHECK(meta["revision"] == Json(fixture.GetEditor().GetRevision()));
			CHECK(meta["dirty"] == Json(true));
			CHECK(meta["undoLabel"].dump().contains("[agent]"));
			CHECK(meta["playState"] == Json("Edit"));
			CHECK(meta["diagnostics"].contains("logCursor"));
		}

		TEST_CASE("AutomationServer: disconnecting a client cancels its pending operations and appends the event")
		{
			Test::EditorTestFixture fixture("ServerDisconnect");
			fixture.CreateAndOpenProject(); // log.read, which checks the cancellation below, needs a project (§12.1)
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

		TEST_CASE("AutomationServer: listening writes a session file that is removed on destruction")
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

		TEST_CASE("AutomationServer: a listening server without a project names no project and serves TCP clients through its dispatcher")
		{
			Test::EditorTestFixture fixture("ServerTcp");
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.Listen = true;
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			const std::filesystem::path file = SessionFile::GetPath(specification.SessionsDirectory, Process::GetCurrentId());
			{
				Result<Scope<AutomationServer>> created = AutomationServer::Create(fixture.GetEditor(), specification);
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				AutomationServer& server = **created;
				const Result<SessionFileContent> content = SessionFile::FromText(FileSystem::ReadText(file).value_or(std::string()));
				REQUIRE(content.has_value());
				CHECK(content->ProjectPath.empty());
				CHECK(content->ProtocolVersionText == "1.0");

				// A client that says hello with the session file's token is admitted on the I/O thread, joins the server's
				// clients at the next pump and is answered through the dispatcher.
				Result<Socket> socket = Socket::Connect(server.GetPort(), std::chrono::seconds(10));
				REQUIRE(socket.has_value());
				const std::string hello = std::format(R"({{"jsonrpc":"2.0","id":1,"method":"session.hello","params":{{"token":"{}","protocolVersion":"1.0","client":{{"name":"engine-tests","version":"1"}}}}}})",
					content->Token);
				const std::string helloFrame = EncodeFrame(hello);
				REQUIRE(socket->Send(std::as_bytes(std::span(helloFrame.data(), helloFrame.size())), std::chrono::seconds(10)).has_value());
				const std::string unknownFrame = EncodeFrame(R"({"jsonrpc":"2.0","id":2,"method":"nosuch.method","params":{}})");
				REQUIRE(socket->Send(std::as_bytes(std::span(unknownFrame.data(), unknownFrame.size())), std::chrono::seconds(10)).has_value());

				// Pumped as the editor's frame does, until both answers arrived (Test::WaitUntil bounds a failure); each turn
				// waits up to 1 ms for the socket to become readable.
				FrameDecoder decoder;
				std::vector<Json> responses;
				std::string failure;
				const bool answered = Test::WaitUntil([&server, &socket, &decoder, &responses, &failure]()
				{
					server.Pump();
					const std::array<const Socket*, 1> readers = { &*socket };
					const Result<SocketReadiness> ready = Socket::WaitAny(readers, nullptr, std::chrono::milliseconds(1));
					if (!ready)
					{
						failure = ready.error().ToString();
						return true;
					}
					if (ready->ReadableSockets.empty())
						return false;
					std::array<std::byte, 4096> buffer{};
					const Result<size_t> received = socket->Receive(buffer, std::chrono::seconds(10));
					if (!received || *received == 0)
					{
						failure = received ? std::string("the server closed the connection") : received.error().ToString();
						return true;
					}
					decoder.Append(std::span(buffer.data(), *received));
					while (true)
					{
						Result<std::optional<std::string>> payload = decoder.Next();
						if (!payload)
						{
							failure = payload.error().ToString();
							return true;
						}
						if (!payload->has_value())
							break;
						Result<Json> json = JsonReader::Parse(**payload);
						if (!json)
						{
							failure = json.error().ToString();
							return true;
						}
						responses.push_back(std::move(*json));
					}
					return responses.size() >= 2;
				});
				REQUIRE(answered);
				REQUIRE_MESSAGE(failure.empty(), failure);
				REQUIRE(responses.size() == 2);
				CHECK(responses[0]["id"] == Json(1));
				CHECK(responses[1]["id"] == Json(2));
				CHECK(responses[1]["error"]["code"] == Json(-32601)); // MethodNotFound, with _meta: it ran on the main thread
				CHECK(responses[1]["error"]["data"].contains("_meta"));
				const std::vector<AutomationClientInfo> clients = server.GetClients();
				REQUIRE(clients.size() == 1);
				CHECK(clients[0].Name == "engine-tests");
				CHECK_FALSE(clients[0].InProcess);

				// The disconnect reaches the server at a later pump, once the I/O thread has seen the socket close.
				socket->Close();
				CHECK(Test::WaitUntil([&server]()
				{
					server.Pump();
					return server.GetClients().empty();
				}));
			}
			CHECK_FALSE(FileSystem::Exists(file));
		}

		TEST_CASE("AutomationServer: in-process clients get their own responses and unknown methods are answered with _meta")
		{
			Test::EditorTestFixture fixture("ServerInProcess");
			Test::AutomationTestClient client(fixture.GetEditor());
			AutomationServer& server = client.GetServer();
			CHECK(server.GetPort() == 0);
			const ClientId other = server.ConnectInProcess("other");
			CHECK(other != client.GetClient());
			server.SubmitInProcess(other, RpcRequest{ .Id = Json(7), .IsNotification = false, .Method = "nosuch.method", .Params = Json::object(), .TranscriptLine = std::nullopt });
			server.Pump();
			CHECK(server.TakeInProcessResponses(client.GetClient()).empty());
			std::vector<Json> responses = server.TakeInProcessResponses(other);
			REQUIRE(responses.size() == 1);
			CHECK(responses[0]["id"] == Json(7));
			CHECK(responses[0]["error"]["code"] == Json(-32601));
			CHECK(responses[0]["error"]["data"]["_meta"]["playState"] == Json("Edit"));
			CHECK(server.GetClients().size() == 2);
			server.DisconnectInProcess(other);
			CHECK(server.GetClients().size() == 1);
		}

		TEST_CASE("AutomationServer: offloaded results land in Library/Automation/Out")
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

		TEST_CASE("AutomationServer: offloaded results of servers that are gone are pruned and a server removes its own on destruction")
		{
			Test::EditorTestFixture fixture("ServerOffloadCleanup");
			fixture.CreateAndOpenProject();
			const std::filesystem::path out = fixture.GetProjectRoot() / "Library" / "Automation" / "Out";
			REQUIRE(FileSystem::CreateDirectories(out).has_value());
			// A process id no process has (above every host's pid range), the id of this running process with another start
			// time (another server of a live process), and a file that is not an offloaded result.
			const std::filesystem::path stale = out / "2147483644-1700000000-00000001.json";
			const std::filesystem::path live = out / std::format("{}-1-00000001.json", Process::GetCurrentId());
			const std::filesystem::path other = out / "Notes.txt";
			for (const std::filesystem::path& file : { stale, live, other })
				REQUIRE(FileSystem::WriteFileAtomic(file, std::as_bytes(std::span("{}", 2))).has_value());

			std::string written;
			{
				Test::AutomationTestClient client(fixture.GetEditor());
				// The unfiltered catalogue is over the offload threshold (ADR 0008 decision 31).
				Result<Json> discovered = client.Call("rpc.discover", Json::object());
				REQUIRE(discovered.has_value());
				REQUIRE((*discovered)["truncated"] == Json(true));
				written = JsonReader((*discovered)["path"]).ReadString().value_or(std::string());
				CHECK(FileSystem::Exists(FileSystem::PathFromUtf8(written)));
				CHECK_FALSE(FileSystem::Exists(stale));
				CHECK(FileSystem::Exists(live));
				CHECK(FileSystem::Exists(other));
			}
			CHECK_FALSE(FileSystem::Exists(FileSystem::PathFromUtf8(written)));
			CHECK(FileSystem::Exists(live));
			CHECK(FileSystem::Exists(other));
		}

		TEST_CASE("AutomationServer: a failed session-file rewrite is reported once and tried again until it succeeds")
		{
			Test::EditorTestFixture fixture("ServerSessionRetry");
			AutomationServerSpecification specification = Test::MakeTestServerSpecification();
			specification.Listen = true;
			specification.SessionsDirectory = fixture.GetDirectory() / "Sessions";
			specification.SessionFileRetryInterval = std::chrono::milliseconds(0);
			const std::filesystem::path file = SessionFile::GetPath(specification.SessionsDirectory, Process::GetCurrentId());
			Result<Scope<AutomationServer>> created = AutomationServer::Create(fixture.GetEditor(), specification);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			AutomationServer& server = **created;

			// A directory in the file's place makes the atomic replace fail, as a reader holding the file open does on
			// Windows.
			REQUIRE(FileSystem::Remove(file).has_value());
			REQUIRE(FileSystem::CreateDirectories(file).has_value());
			fixture.CreateAndOpenProject();
			{
				const Test::ExpectLog warned(LogLevel::Warn, "Could not rewrite the automation session file");
				server.Pump();
				server.Pump();
				CHECK(warned.GetMatchCount() == 1);
			}

			REQUIRE(FileSystem::Remove(file).has_value());
			server.Pump();
			const Result<SessionFileContent> rewritten = SessionFile::FromText(FileSystem::ReadText(file).value_or(std::string()));
			REQUIRE(rewritten.has_value());
			CHECK(rewritten->ProjectPath.ends_with("TestProject.eproj"));
		}
	}

}
