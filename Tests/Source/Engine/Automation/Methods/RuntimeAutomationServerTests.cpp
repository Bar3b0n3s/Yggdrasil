#include "TestsPCH.h"

#include "Engine/Automation/Methods/RuntimeAutomationServer.h"

#include "Engine/Asset/AssetTypeRegistration.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/RegisterSharedMethods.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/SessionFile.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Mounts/MemoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ExpectLog.h"
#include "Support/SceneTestFixture.h"
#include "Support/TempDirectory.h"
#include "Support/WaitUntil.h"

#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>

// The Runtime's automation server (Architecture §13.2, §13.5 "Runtime subset"; Docs/Decisions/0012-m7-decisions.md
// decision 12) in process: the shared handlers on the Runtime's context, over a play session of a test scene.

namespace Engine {

	namespace {

		// A registry as the Runtime's context builds it (RuntimeApp: RegisterAutomationSharedTypes and
		// RegisterSharedMethodTypes on top of the context's own types), frozen.
		Scope<TypeRegistry> CreateRuntimeRegistry()
		{
			Scope<TypeRegistry> registry = CreateScope<TypeRegistry>();
			RegisterBuiltinComponents(*registry);
			RegisterProjectSettingsTypes(*registry);
			RegisterAssetTypes(*registry);
			RegisterAutomationSharedTypes(*registry);
			RegisterSharedMethodTypes(*registry);
			registry->Freeze();
			return registry;
		}

		// A Runtime server over a session of a scene with a "Ball" under "Game" (and whatever `build` adds before the session
		// starts), and an in-process client.
		class RuntimeServerFixture
		{
		public:
			explicit RuntimeServerFixture(RuntimeAutomationServerSpecification specification = { .GameName = "Tiny" },
				const std::function<void(Scene&)>& build = {})
				: m_Registry(CreateRuntimeRegistry())
			{
				Scene& scene = m_Scene.GetScene();
				Entity game = scene.CreateEntity("Game");
				static_cast<void>(scene.CreateEntity("Ball", game));
				if (build)
					build(scene);
				PlaySessionSpecification session;
				session.Registry = m_Registry.get();
				Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(session, scene);
				REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
				m_Session = std::move(*created);
				REQUIRE(m_Vfs.Mount("user", CreateScope<MemoryMount>()).has_value());
				Result<Scope<RuntimeAutomationServer>> server = RuntimeAutomationServer::Create(*m_Registry, m_Events, m_Vfs, *m_Session, specification);
				REQUIRE_MESSAGE(server.has_value(), server.error().ToString());
				m_Server = std::move(*server);
				m_Client = m_Server->ConnectInProcess("test");
			}

			[[nodiscard]] RuntimeAutomationServer& GetServer() { return *m_Server; }
			[[nodiscard]] PlaySession& GetSession() { return *m_Session; }
			[[nodiscard]] EventLog& GetEvents() { return m_Events; }
			[[nodiscard]] ClientId GetClient() const { return m_Client; }
			[[nodiscard]] Result<PlaySession*> Restart(const PlayStartOptions& options)
			{
				PlaySessionSpecification spec;
				spec.Registry = m_Registry.get();
				spec.Serial = m_Session->GetSerial() + 1;
				spec.Seed = options.Seed.value_or(0);
				spec.Parameters = options.Parameters;
				ENGINE_TRY_ASSIGN(auto replacement, PlaySession::CreateFromScene(spec, m_Scene.GetScene()));
				replacement->SetLockstep(options.Lockstep, options.LockstepOwner);
				replacement->SetPaused(options.Paused);
				m_Session = std::move(replacement);
				return m_Session.get();
			}
			[[nodiscard]] ReplayHeader DescribeHeader() const
			{
				ReplayHeader header;
				header.Scene = { UUID(42), "Assets/Scenes/Main.scene" };
				header.Seed = m_Session->GetSeed();
				header.FixedHz = m_Session->GetProjectSettings().Simulation.FixedHz;
				header.Parameters.Set(m_Session->GetLoadParameters());
				header.EngineVersion = "1";
				header.Config = "Debug";
				return header;
			}

			// One request of the fixture's client, answered within the pump that runs it (none of these methods is pending):
			// returns the whole response object, with "result" or "error".
			[[nodiscard]] Json Call(std::string method, Json params = Json::object())
			{
				m_Server->SubmitInProcess(m_Client, RpcRequest{ .Id = Json(++m_NextId), .Method = std::move(method), .Params = std::move(params), .TranscriptLine = std::nullopt });
				m_Server->Pump();
				std::vector<Json> responses = m_Server->TakeInProcessResponses(m_Client);
				REQUIRE(responses.size() == 1);
				return std::move(responses.front());
			}
			[[nodiscard]] Json CallPending(std::string method, Json params)
			{
				m_Server->SubmitInProcess(m_Client, RpcRequest{ .Id = Json(++m_NextId), .Method = std::move(method), .Params = std::move(params), .TranscriptLine = std::nullopt });
				std::vector<Json> responses;
				REQUIRE(Test::WaitUntil([this, &responses]
				{
					m_Server->Pump();
					responses = m_Server->TakeInProcessResponses(m_Client);
					return !responses.empty();
				}));
				REQUIRE(responses.size() == 1);
				return std::move(responses.front());
			}

			// The error code of a response, or 0 for a result.
			[[nodiscard]] static int64_t GetErrorCode(const Json& response)
			{
				const auto error = response.find("error");
				if (error == response.end())
					return 0;
				return JsonReader(*error).ReadMember<int64_t>("code").value_or(0);
			}
		private:
			Test::SceneTestFixture m_Scene;
			Scope<TypeRegistry> m_Registry;
			Scope<PlaySession> m_Session;
			EventLog m_Events;
			VirtualFileSystem m_Vfs;
			Scope<RuntimeAutomationServer> m_Server;
			ClientId m_Client = NoClient;
			int64_t m_NextId = 0;
		};

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("RuntimeAutomationServer: script evaluation and waits use the live session and retained error service")
		{
			RuntimeServerFixture fixture;
			fixture.GetSession().SetPaused(true);
			CHECK(fixture.Call("script.eval", Json{ { "context", "play" }, { "code", "return 2 + 3" } })["result"]["value"] == Json(5));
			CHECK(fixture.Call("script.eval", Json{ { "context", "edit" }, { "code", "1" } })["error"]["data"]["errorCode"] == Json("Unsupported"));
			const Json cursor = fixture.Call("script.errors", Json{ { "since", "end" } })["result"]["nextCursor"];
			{
				Test::ExpectLog expected(LogLevel::Error, "runtime native fault");
				CHECK(fixture.Call("script.eval", Json{ { "context", "play" }, { "code", "error('runtime native fault')" } })["error"]["data"]["errorCode"] == Json("Script"));
			}
			const Json errors = fixture.Call("script.errors", Json{ { "since", cursor } });
			REQUIRE(errors["result"]["errors"].size() == 1);
			CHECK(errors["result"]["errors"][0]["message"].dump().contains("runtime native fault"));
			const Json waited = fixture.CallPending("play.waitFor", Json{ { "until", "Time.GetTick() == 2" }, { "timeoutTicks", 4 } });
			REQUIRE_MESSAGE(waited.contains("result"), waited.dump());
			CHECK(waited["result"]["satisfied"] == Json(true));
			CHECK(waited["result"]["tick"] == Json(2));
		}

		TEST_CASE("RuntimeAutomationServer: recording and replay callbacks rebind the session and preserve process errors")
		{
			RuntimeServerFixture* active = nullptr; // Set before any request, and outlives the server callbacks.
			ScriptErrorStream errors;
			static_cast<void>(errors.Add({ .Script = "Assets/Scripts/Old.luau", .Line = 3, .Message = "retained old VM fault" }));
			bool writable = false;
			std::optional<ReplayDocument> saved;
			std::vector<uint64_t> released;
			RuntimeAutomationServerSpecification spec;
			spec.GameName = "Tiny";
			spec.ScriptErrors = &errors;
			spec.StartRecordingSession = [&active](const PlayStartOptions& options, bool restart) -> Result<PlaySession*>
			{
				CHECK(restart);
				return active->Restart(options);
			};
			spec.DescribeReplayHeader = [&active]() -> Result<ReplayHeader>
			{
				return active->DescribeHeader();
			};
			spec.ValidateReplayOutput = [&writable](std::string_view path) -> Result<std::string>
			{
				CHECK(path == "Local.replay");
				if (!writable)
					return MakeError(ErrorCode::PermissionDenied, "test output is read-only");
				return "user://Replays/" + std::string(path);
			};
			spec.WriteReplay = [&saved](std::string_view path, const ReplayDocument& document) -> Result<std::string>
			{
				saved = document;
				return "user://Replays/" + std::string(path);
			};
			spec.LoadReplay = [&saved](std::string_view path) -> Result<AssetRef<ReplayData>>
			{
				CHECK(path == "user://Replays/Local.replay");
				if (!saved)
					return MakeError(ErrorCode::NotFound, "no recording saved");
				auto replay = CreateRef<ReplayData>();
				replay->Header = saved->Header;
				replay->Events = saved->Events;
				replay->FinalTick = saved->FinalTick;
				replay->FinalStateHash = saved->FinalStateHash;
				return AssetRef<ReplayData>(std::move(replay));
			};
			spec.RestartForReplay = [&active](const ReplayHeader& header) -> Result<PlaySession*>
			{
				PlayStartOptions options;
				options.Seed = header.Seed;
				options.Parameters = header.Parameters;
				options.Lockstep = true;
				return active->Restart(options);
			};
			spec.ReleaseReplayInput = [&released](uint64_t serial)
			{
				released.push_back(serial);
			};
			RuntimeServerFixture fixture(spec);
			active = &fixture;
			const uint64_t initial = fixture.GetSession().GetSerial();
			const Json started = fixture.Call("input.record", Json{ { "action", "start" }, { "restart", true }, { "seed", 59 }, { "parameters", Json{ { "level", 7 } } } });
			REQUIRE_MESSAGE(started.contains("result"), started.dump());
			CHECK(fixture.GetSession().GetSerial() != initial);
			CHECK(fixture.GetSession().GetSeed() == 59);
			CHECK(fixture.GetSession().GetLoadParameters() == Json({ { "level", 7 } }));
			CHECK(fixture.Call("script.errors")["result"]["errors"].size() == 1);
			REQUIRE(fixture.CallPending("play.step", Json{ { "ticks", 2 }, { "render", "none" } }).contains("result"));
			const Json refused = fixture.Call("input.record", Json{ { "action", "stop" }, { "path", "Local.replay" } });
			CHECK(refused["error"]["data"]["errorCode"] == Json("PermissionDenied"));
			REQUIRE(fixture.GetSession().GetRecorder() != nullptr);
			CHECK(fixture.GetSession().GetRecorder()->IsRecording());
			CHECK_FALSE(saved.has_value());
			writable = true;
			const Json stopped = fixture.Call("input.record", Json{ { "action", "stop" }, { "path", "Local.replay" } });
			REQUIRE_MESSAGE(stopped.contains("result"), stopped.dump());
			REQUIRE(saved.has_value());
			CHECK(saved->Header.Seed == 59);
			CHECK(saved->FinalTick == 2);
			const uint64_t recordedSerial = fixture.GetSession().GetSerial();
			const Json replayed = fixture.CallPending("input.replay", Json{ { "path", stopped["result"]["path"] }, { "verify", true }, { "strictHash", true } });
			REQUIRE_MESSAGE(replayed.contains("result"), replayed.dump());
			CHECK(replayed["result"]["stateHash"] == stopped["result"]["stateHash"]);
			CHECK(fixture.GetSession().GetSerial() != recordedSerial);
			CHECK(fixture.GetSession().IsPaused());
			CHECK_FALSE(fixture.GetSession().IsLockstep());
			REQUIRE(released.size() == 1);
			CHECK(released.front() == fixture.GetSession().GetSerial());
			CHECK(fixture.Call("script.errors")["result"]["errors"].size() == 1);
		}

		TEST_CASE("RuntimeAutomationServer: a recording owner's disconnect cancels after play.pause releases lockstep")
		{
			RuntimeServerFixture* active = nullptr;
			RuntimeAutomationServerSpecification spec;
			spec.StartRecordingSession = [&active](const PlayStartOptions& options, bool)
			{
				return active->Restart(options);
			};
			spec.DescribeReplayHeader = [&active]() -> Result<ReplayHeader>
			{
				return active->DescribeHeader();
			};
			RuntimeServerFixture fixture(spec);
			active = &fixture;
			REQUIRE(fixture.Call("input.record", Json{ { "action", "start" }, { "restart", true } }).contains("result"));
			REQUIRE(fixture.Call("play.pause").contains("result"));
			CHECK_FALSE(fixture.GetSession().IsLockstep());
			REQUIRE(fixture.GetSession().GetRecorder()->IsRecording());
			fixture.GetServer().DisconnectInProcess(fixture.GetClient());
			CHECK_FALSE(fixture.GetSession().GetRecorder()->IsRecording());
			CHECK(fixture.GetSession().IsPaused());
		}

		TEST_CASE("RuntimeAutomationServer: failed restart preserves the original live session")
		{
			RuntimeAutomationServerSpecification spec;
			spec.StartRecordingSession = [](const PlayStartOptions&, bool) -> Result<PlaySession*>
			{
				return MakeError(ErrorCode::Validation, "invalid replacement scene");
			};
			RuntimeServerFixture fixture(spec);
			const uint64_t serial = fixture.GetSession().GetSerial();
			const Json result = fixture.Call("input.record", Json{ { "action", "start" }, { "restart", true } });
			CHECK(result["error"]["data"]["errorCode"] == Json("Validation"));
			CHECK(fixture.GetSession().GetSerial() == serial);
			CHECK_FALSE(fixture.GetSession().GetRecorder()->IsRecording());
			CHECK(fixture.Call("scene.tree", Json{ { "format", "json" } })["result"]["entities"].size() == 2);
		}

		TEST_CASE("RuntimeAutomationServer: disconnect cancels a pending replay and releases its input suppression once")
		{
			RuntimeServerFixture* active = nullptr;
			std::vector<uint64_t> released;
			auto now = std::chrono::steady_clock::time_point{};
			RuntimeAutomationServerSpecification spec;
			spec.WallClock = [&now]
			{
				now += std::chrono::milliseconds(100);
				return now;
			};
			spec.LoadReplay = [&active](std::string_view) -> Result<AssetRef<ReplayData>>
			{
				auto replay = CreateRef<ReplayData>();
				replay->Header = active->DescribeHeader();
				replay->FinalTick = 1000;
				replay->FinalStateHash = "0000000000000000";
				ReplayEvent event;
				event.Tick = 50;
				event.Type = "Key";
				event.KeyName = "A";
				event.State = "Down";
				replay->Events.push_back(event);
				event.Tick = 51;
				event.State = "Up";
				replay->Events.push_back(event);
				event.Tick = 60;
				event.KeyName = "B";
				event.State = "Tap";
				replay->Events.push_back(event);
				return AssetRef<ReplayData>(std::move(replay));
			};
			spec.RestartForReplay = [&active](const ReplayHeader& header)
			{
				PlayStartOptions options;
				options.Seed = header.Seed;
				options.Parameters = header.Parameters;
				options.Lockstep = true;
				return active->Restart(options);
			};
			spec.ReleaseReplayInput = [&released](uint64_t serial)
			{
				released.push_back(serial);
			};
			RuntimeServerFixture fixture(spec);
			active = &fixture;
			auto& server = fixture.GetServer();
			server.SubmitInProcess(fixture.GetClient(), RpcRequest{ .Id = Json(1), .Method = "input.replay", .Params = Json{ { "path", "Assets/Replays/Long.replay" } }, .TranscriptLine = std::nullopt });
			server.Pump();
			CHECK(server.TakeInProcessResponses(fixture.GetClient()).empty());
			CHECK(fixture.GetSession().IsStepping());
			const uint64_t tick = fixture.GetSession().GetTick();
			REQUIRE(tick < 50);
			server.DisconnectInProcess(fixture.GetClient());
			CHECK(fixture.GetSession().IsPaused());
			CHECK_FALSE(fixture.GetSession().IsLockstep());
			CHECK_FALSE(fixture.GetSession().IsStepping());
			REQUIRE(released.size() == 1);
			CHECK(released.front() == fixture.GetSession().GetSerial());
			const ClientId resumed = server.ConnectInProcess("resumed");
			server.SubmitInProcess(resumed, RpcRequest{ .Id = Json(1), .Method = "input.inject", .Params = Json{ { "events", Json::array({ Json{ { "tick", 50 - tick }, { "type", "key" }, { "key", "C" }, { "state", "tap" } } }) } }, .TranscriptLine = std::nullopt });
			server.Pump();
			const auto responses = server.TakeInProcessResponses(resumed);
			REQUIRE(responses.size() == 1);
			REQUIRE_MESSAGE(responses.front().contains("result"), responses.front().dump());
			for (uint64_t applied = tick; applied < 65; ++applied)
			{
				fixture.GetSession().Tick();
				REQUIRE(fixture.GetSession().GetTick() == applied + 1);
				CHECK_FALSE(fixture.GetSession().GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::A));
				CHECK_FALSE(fixture.GetSession().GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::B));
				CHECK(fixture.GetSession().GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::C) == (applied == 50));
			}
			server.DisconnectInProcess(resumed);
			CHECK(released.size() == 1);
		}

		TEST_CASE("RuntimeAutomationServer: serves the Runtime subset and only it")
		{
			RuntimeServerFixture fixture;
			const MethodRegistry& methods = fixture.GetServer().GetMethods();
			for (const char* name : { "session.hello", "session.info", "session.shutdown", "rpc.discover", "scene.tree", "scene.query", "scene.get",
					 "entity.get", "entity.bounds", "log.read", "events.read", "play.pause", "play.resume", "play.step", "play.state",
					 "play.setTimeScale", "input.inject", "viewport.screenshot", "physics.bodyInfo", "audio.stats", "stats.get",
					 "script.eval", "script.errors", "play.waitFor", "input.record", "input.replay" })
			{
				INFO(std::string(name));
				const MethodDescriptor* method = methods.Find(name);
				REQUIRE(method != nullptr);
				CHECK(method->Specification.AvailableInRuntime);
			}
			// Editor-only methods are not served.
			for (const char* name : { "play.start", "play.stop", "entity.create", "entity.update", "scene.new", "scene.diff", "docs.get", "project.export", "test.run", "script.write", "script.check", "script.reload" })
			{
				INFO(std::string(name));
				CHECK(methods.Find(name) == nullptr);
			}
		}

		TEST_CASE("RuntimeAutomationServer: play.step in process advances the Runtime's session")
		{
			RuntimeServerFixture fixture;
			RuntimeAutomationServer& server = fixture.GetServer();
			const ClientId client = fixture.GetClient();
			server.SubmitInProcess(client, RpcRequest{ .Id = Json(1), .Method = "play.pause", .Params = Json::object(), .TranscriptLine = std::nullopt });
			server.SubmitInProcess(client, RpcRequest{ .Id = Json(2), .Method = "play.step", .Params = Json{ { "ticks", 5 } }, .TranscriptLine = std::nullopt });
			std::vector<Json> responses;
			for (int pump = 0; pump < 100 && responses.size() < 2; ++pump)
			{
				server.Pump();
				for (Json& response : server.TakeInProcessResponses(client))
					responses.push_back(std::move(response));
			}
			REQUIRE(responses.size() == 2);
			CHECK(responses[1]["result"]["tick"] == Json(5));
			CHECK(fixture.GetSession().GetTick() == 5);
			// "_meta" reports the session's state (§13.4).
			CHECK(responses[1]["result"]["_meta"]["playState"] == Json("Paused"));
			CHECK(responses[1]["result"]["_meta"]["tick"] == Json(5));
		}

		TEST_CASE("RuntimeAutomationServer: session.info reports the game, its play state and its clients")
		{
			RuntimeServerFixture fixture({ .GameName = "Tiny", .Headless = true, .RendererName = "none" });
			const Json info = fixture.Call("session.info");
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(info) == 0, info.dump());
			const Json& result = info["result"];
			CHECK(result["project"]["open"] == Json(true));
			CHECK(result["project"]["name"] == Json("Tiny"));
			CHECK(result["project"]["projectFile"] == Json(""));
			CHECK(result["readOnly"] == Json(true));
			CHECK(result["headless"] == Json(true));
			CHECK(result["renderer"] == Json("none"));
			CHECK(result["playState"] == Json("Play"));
			CHECK(result["pid"] == Json(Process::GetCurrentId()));
			REQUIRE(result["clients"].size() == 1);
			CHECK(result["clients"][0]["name"] == Json("test"));
			CHECK(result["capabilities"].dump().contains("pendingOperations"));
			CHECK_FALSE(result["capabilities"].dump().contains("dryRun"));

			fixture.GetSession().SetPaused(true);
			CHECK(fixture.Call("session.info")["result"]["playState"] == Json("Paused"));
		}

		TEST_CASE("RuntimeAutomationServer: the scene and entity reads address the play scene")
		{
			// The Runtime has no edit scene (§13.4 "Target").
			RuntimeServerFixture fixture;
			const Json tree = fixture.Call("scene.tree", Json{ { "format", "Json" } });
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(tree) == 0, tree.dump());
			CHECK(tree["result"]["entities"].size() == 2);
			CHECK(tree["result"]["scene"]["entityCount"] == Json(2));
			CHECK(tree["result"]["scene"]["dirty"] == Json(false));

			const Json ball = fixture.Call("entity.get", Json{ { "entity", "/Game/Ball" }, { "target", "play" } });
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(ball) == 0, ball.dump());
			CHECK(ball["result"]["entity"]["name"] == Json("Ball"));

			const Json query = fixture.Call("scene.query", Json{ { "where", Json{ { "name", "B*" } } } });
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(query) == 0, query.dump());
			CHECK(query["result"]["total"] == Json(1));

			const Json document = fixture.Call("scene.get");
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(document) == 0, document.dump());
			CHECK(document["result"]["scene"]["Format"] == Json("Scene"));

			const Json edit = fixture.Call("scene.tree", Json{ { "target", "edit" } });
			CHECK(RuntimeServerFixture::GetErrorCode(edit) == static_cast<int64_t>(RpcErrorCode::InvalidState));
			CHECK(edit["error"]["data"]["issues"][0]["pointer"] == Json("/target"));

			// Without the game's asset manager entity.bounds cannot read meshes.
			const Json bounds = fixture.Call("entity.bounds", Json{ { "entities", Json::array({ "/Game" }) } });
			CHECK(RuntimeServerFixture::GetErrorCode(bounds) == static_cast<int64_t>(RpcErrorCode::Unsupported));
		}

		TEST_CASE("RuntimeAutomationServer: physics.bodyInfo answers for an entity of the Runtime's session")
		{
			// The ball has no collider, so no body (§13.5: physics.bodyInfo is in the Runtime subset).
			RuntimeServerFixture fixture;
			const Json ball = fixture.Call("physics.bodyInfo", Json{ { "entity", "/Game/Ball" } });
			CHECK(RuntimeServerFixture::GetErrorCode(ball) == static_cast<int64_t>(RpcErrorCode::NotFound));
			CHECK(ball["error"]["data"]["issues"][0]["pointer"] == Json("/entity"));
			const Json missing = fixture.Call("physics.bodyInfo", Json{ { "entity", "/Nobody" } });
			CHECK(RuntimeServerFixture::GetErrorCode(missing) == static_cast<int64_t>(RpcErrorCode::NotFound));
		}

		TEST_CASE("RuntimeAutomationServer: physics.bodyInfo reports a body of the Runtime's session")
		{
			// A floor with a Static RigidBody and a box collider: the Runtime's session created its body.
			RuntimeServerFixture fixture({ .GameName = "Tiny" }, [](Scene& scene)
			{
				Entity ground = scene.CreateEntity("Floor");
				ground.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
				ground.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(5.0f, 0.5f, 5.0f) });
			});
			const Json info = fixture.Call("physics.bodyInfo", Json{ { "entity", "/Floor" } });
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(info) == 0, info.dump());
			const Json& result = info["result"];
			CHECK(result["entity"]["name"] == Json("Floor"));
			CHECK(result["body"]["id"] == result["entity"]["id"]);
			CHECK(result["origin"] == Json("RigidBody"));
			CHECK(result["type"] == Json("Static"));
			CHECK(result["layer"] == Json("Default"));
			CHECK(result["colliders"].size() == 1);
			CHECK(result["contacts"].empty());
			CHECK(result["tick"] == Json(0));
			CHECK(JsonReader(result["boundsMax"][0]).ReadFloat().value_or(0.0f) == doctest::Approx(5.0f));
		}

		TEST_CASE("RuntimeAutomationServer: a disconnect releases the client's lockstep and pauses play")
		{
			// §13.2 "Disconnect".
			RuntimeServerFixture fixture;
			PlaySession& session = fixture.GetSession();
			const uint64_t cursor = fixture.GetEvents().GetNextSeq();
			session.SetLockstep(true, fixture.GetClient());
			fixture.GetServer().DisconnectInProcess(fixture.GetClient());
			CHECK_FALSE(session.IsLockstep());
			CHECK(session.IsPaused());
			const EventReadResult events = fixture.GetEvents().Read(cursor);
			REQUIRE(events.Events.size() == 2);
			CHECK(events.Events[0].Type == EngineEventType::PlayStateChanged);
			CHECK(events.Events[0].Name == "Paused");
			CHECK(events.Events[1].Type == EngineEventType::AutomationClientDisconnected);
			CHECK(events.Events[1].Name == "test");
		}

		TEST_CASE("RuntimeAutomationServer: session.shutdown requests the exit")
		{
			// The Runtime saves nothing: it has no scene file.
			RuntimeServerFixture fixture;
			CHECK_FALSE(fixture.GetServer().GetShutdownRequest().has_value());
			const Json shutdown = fixture.Call("session.shutdown", Json{ { "save", true } });
			REQUIRE_MESSAGE(RuntimeServerFixture::GetErrorCode(shutdown) == 0, shutdown.dump());
			CHECK(shutdown["result"]["saved"] == Json(false));
			CHECK(fixture.GetServer().GetShutdownRequest() == std::optional<int>(0));
		}

		TEST_CASE("RuntimeAutomationServer: a listening server writes its session file under the game's name")
		{
			// §13.2: a client finds an exported game by its name.
			const Test::TempDirectory directory("RuntimeSessionFile");
			const std::filesystem::path sessions = directory / "Tiny/Automation/Sessions";
			{
				RuntimeServerFixture fixture({ .Listen = true, .GameName = "Tiny", .SessionsDirectory = sessions });
				CHECK(fixture.GetServer().GetPort() != 0);
				const Result<std::string> text = FileSystem::ReadText(SessionFile::GetPath(sessions, Process::GetCurrentId()));
				REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
				CHECK(text->contains("\"projectPath\": \"Tiny\""));
			}
			std::error_code error;
			CHECK_FALSE(std::filesystem::exists(SessionFile::GetPath(sessions, Process::GetCurrentId()), error));
		}

		// M12 (Docs/Decisions/0015-m12-decisions.md): audio.stats is in the Runtime subset (§13.5), listed in "serves the Runtime
		// subset and only it".
		TEST_CASE("RuntimeAutomationServer: audio.stats reports the game's audio engine")
		{
			VirtualFileSystem vfs;
			Result<Scope<AudioEngine>> audio = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, vfs);
			REQUIRE_MESSAGE(audio.has_value(), audio.error().ToString());
			{
				RuntimeServerFixture fixture({ .GameName = "Tiny", .Audio = audio->get() });
				const MethodDescriptor* method = fixture.GetServer().GetMethods().Find("audio.stats");
				REQUIRE(method != nullptr);
				CHECK(method->Specification.AvailableInRuntime);
				const Json stats = fixture.Call("audio.stats");
				REQUIRE(stats.contains("result"));
				CHECK(stats["result"]["deviceState"] == Json("None"));
				CHECK(stats["result"]["voiceCount"] == Json(0));
			}
			RuntimeServerFixture silent;
			CHECK(RuntimeServerFixture::GetErrorCode(silent.Call("audio.stats")) != 0);
		}
	}

}
