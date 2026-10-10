#include "TestsPCH.h"

#include "Engine/Automation/Methods/PlayMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"
#include "Support/EditorTestFixture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// play.* in process, through the editor's server (Architecture §13.5, §13.6; ADR 0008: "an in-process round trip for
// every method"; Docs/Decisions/0012-m7-decisions.md decisions 3 and 8).

namespace Engine {

	namespace {

		// One entity in the fixture's open scene, so play has something to copy.
		void OpenSceneWithEntity(Test::AutomationFixture& fixture)
		{
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Ball" } }).has_value());
		}

		// Sends one request as `client`, another in-process client of the fixture's server, and pumps until its response.
		Json RequestAs(Test::AutomationFixture& fixture, ClientId client, std::string_view method, const Json& params)
		{
			static int64_t s_NextId = 1000;
			AutomationServer& server = fixture.GetClient().GetServer();
			const int64_t id = s_NextId++;
			server.SubmitInProcess(client,
				RpcRequest{ .Id = Json(id), .IsNotification = false, .Method = std::string(method), .Params = params, .TranscriptLine = std::nullopt });
			for (int pump = 0; pump < 1000; ++pump)
			{
				server.Pump();
				for (Json& response : server.TakeInProcessResponses(client))
				{
					if (response["id"] == Json(id))
						return std::move(response);
				}
			}
			FAIL_CHECK("no response to '" << std::string(method) << "'");
			return Json();
		}

		// The engine error code name of an error response ("" for a success).
		std::string GetErrorCode(const Json& response)
		{
			if (!response.contains("error"))
				return {};
			return JsonReader(response["error"]["data"]["errorCode"]).ReadString().value_or(std::string());
		}

		// The wall clock of a server the test drives (AutomationServerSpecification::WallClock): every reading advances it by
		// Step, so a play.step frame runs exactly the ticks whose readings fit PlayStepFrameBudget.
		struct ScriptedWallClock
		{
			std::chrono::steady_clock::time_point Now{};
			std::chrono::milliseconds Step{ 0 };
		};

		// A project with a scene holding the entity Ball, and a client of a server on `clock` (which outlives it).
		class ScriptedClockSetup
		{
		public:
			ScriptedClockSetup(std::string_view label, ScriptedWallClock& clock)
				: m_Fixture(label)
			{
				m_Fixture.CreateAndOpenProject();
				m_Fixture.CreateAndOpenScene();
				AutomationServerSpecification specification = Test::MakeTestServerSpecification();
				// A pump runs every queued request (its budget is real time, which a slow Debug run could spend early), so a
				// test decides exactly what happens between two polls of a play.step.
				specification.PumpBudget = std::chrono::minutes(1);
				ScriptedWallClock* scripted = &clock;
				specification.WallClock = [scripted]()
				{
					scripted->Now += scripted->Step;
					return scripted->Now;
				};
				m_Client = CreateScope<Test::AutomationTestClient>(m_Fixture.GetEditor(), std::move(specification));
				REQUIRE(m_Client->Call("entity.create", Json{ { "name", "Ball" } }).has_value());
			}

			[[nodiscard]] Test::AutomationTestClient& GetClient() { return *m_Client; }
			[[nodiscard]] EditorContext& GetEditor() { return m_Fixture.GetEditor(); }

			// play.step's result; fails the test case on an error.
			[[nodiscard]] Json Step(uint32_t ticks, std::string_view render)
			{
				const Result<Json> stepped = m_Client->Call("play.step", Json{ { "ticks", ticks }, { "render", render } });
				REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
				return *stepped;
			}
		private:
			Test::EditorTestFixture m_Fixture;
			Scope<Test::AutomationTestClient> m_Client;
		};

		// Submits `method` for `client` of `server` without pumping.
		int64_t SubmitAs(AutomationServer& server, ClientId client, int64_t id, std::string_view method, const Json& params)
		{
			server.SubmitInProcess(client,
				RpcRequest{ .Id = Json(id), .IsNotification = false, .Method = std::string(method), .Params = params, .TranscriptLine = std::nullopt });
			return id;
		}

		// The names of the PlayStateChanged events appended since `cursor`.
		std::vector<std::string> ReadPlayStateEvents(Test::AutomationFixture& fixture, uint64_t cursor)
		{
			const std::array<EngineEventType, 1> types = { EngineEventType::PlayStateChanged };
			std::vector<std::string> names;
			for (const EngineEvent& event : fixture.GetEditorFixture().GetEngine().GetEventLog().Read(cursor, types).Events)
				names.push_back(event.Name);
			return names;
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("PlayMethods: play.start, play.state and play.stop report the session")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			const Result<Json> started = fixture.Call("play.start", Json{ { "lockstep", true }, { "seed", 7 } });
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			CHECK((*started)["state"] == Json("Play"));
			CHECK((*started)["mode"] == Json("Play"));
			CHECK((*started)["lockstep"] == Json(true));
			CHECK((*started)["ownedByCaller"] == Json(true));
			CHECK((*started)["tick"] == Json(0));
			CHECK(JsonReader((*started)["stateHash"]).ReadString().value_or(std::string()).size() == 16);

			const Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["stateHash"] == (*started)["stateHash"]);

			const Result<Json> stopped = fixture.Call("play.stop", Json::object());
			REQUIRE(stopped.has_value());
			const Result<Json> edit = fixture.Call("play.state", Json::object());
			REQUIRE(edit.has_value());
			CHECK((*edit)["state"] == Json("Edit"));
		}

		TEST_CASE("PlayMethods: play.step runs its ticks and reports the tick and the state hash")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Result<Json> stepped = fixture.Call("play.step", Json{ { "ticks", 120 }, { "render", "none" } });
			REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
			CHECK((*stepped)["tick"] == Json(120));
			CHECK((*stepped)["ticks"] == Json(120));
			CHECK((*stepped)["rendered"] == Json(0));
			CHECK(JsonReader((*stepped)["frames"]).ReadUInt32().value_or(0) >= 1);
			const Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["stateHash"] == (*stepped)["stateHash"]);
		}

		TEST_CASE("PlayMethods: play.step needs lockstep or a paused session, and one at a time")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			const Result<Json> notPlaying = fixture.Call("play.step", Json{ { "ticks", 1 } });
			REQUIRE_FALSE(notPlaying.has_value());
			CHECK(notPlaying.error().GetCode() == ErrorCode::InvalidState);

			REQUIRE(fixture.Call("play.start", Json::object()).has_value());
			const Result<Json> running = fixture.Call("play.step", Json{ { "ticks", 1 } });
			REQUIRE_FALSE(running.has_value());
			CHECK(running.error().GetCode() == ErrorCode::InvalidState);

			REQUIRE(fixture.Call("play.pause", Json::object()).has_value());
			const Result<Json> paused = fixture.Call("play.step", Json{ { "ticks", 3 } });
			REQUIRE_MESSAGE(paused.has_value(), paused.error().ToString());
			const Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["state"] == Json("Paused"));
		}

		TEST_CASE("PlayMethods: play.start accepts load parameters and pauseOnError and rejects non-object parameters")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			for (const Json& parameters : { Json(nullptr), Json(1), Json::array() })
			{
				const Result<Json> refused = fixture.Call("play.start", Json{ { "parameters", parameters } });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues().front().JsonPointer == "/parameters");
			}
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsPlaying());
			const Json parameters{ { "Level", 3 }, { "Nested", Json{ { "ready", true } } } };
			REQUIRE(fixture.Call("play.start", Json{ { "parameters", parameters }, { "pauseOnError", false }, { "lockstep", true } }).has_value());
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetLoadParameters() == parameters);
		}

		TEST_CASE("PlayMethods: play.setTimeScale validates its range")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			REQUIRE(fixture.Call("play.start", Json::object()).has_value());
			REQUIRE(fixture.Call("play.setTimeScale", Json{ { "scale", 0.5 } }).has_value());
			CHECK(fixture.GetEditor().GetPlay().GetFrameTimeScale() == doctest::Approx(0.5));
			const Result<Json> tooFast = fixture.Call("play.setTimeScale", Json{ { "scale", 1000.0 } });
			REQUIRE_FALSE(tooFast.has_value());
			CHECK(tooFast.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("PlayMethods: dryRun is Unsupported for every play method")
		{
			// §13.4: play.* return Unsupported for dryRun, in Edit mode and while playing, and change nothing.
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			const std::vector<std::pair<std::string, Json>> calls = {
				{ "play.start", Json::object() },
				{ "play.stop", Json::object() },
				{ "play.pause", Json::object() },
				{ "play.resume", Json::object() },
				{ "play.step", Json{ { "ticks", 1 } } },
				{ "play.state", Json::object() },
				{ "play.setTimeScale", Json{ { "scale", 1.0 } } },
			};
			for (const bool playing : { false, true })
			{
				if (playing)
					REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
				for (const std::pair<std::string, Json>& call : calls)
				{
					CAPTURE(call.first);
					CAPTURE(playing);
					Json dry = call.second;
					dry["dryRun"] = true;
					const Result<Json> refused = fixture.Call(call.first, dry);
					REQUIRE_FALSE(refused.has_value());
					CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
				}
				CHECK(fixture.GetEditor().GetPlay().IsPlaying() == playing);
			}
			const Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["tick"] == Json(0));
		}

		TEST_CASE("PlayMethods: lockstep belongs to its owner; other clients get InvalidState naming it")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const ClientId other = fixture.GetClient().GetServer().ConnectInProcess("other");
			const std::vector<std::pair<std::string, Json>> refused = {
				{ "play.step", Json{ { "ticks", 1 } } },
				{ "play.stop", Json::object() },
				{ "play.pause", Json::object() },
				{ "play.resume", Json::object() },
				{ "play.setTimeScale", Json{ { "scale", 2.0 } } },
				{ "input.inject", Json{ { "events", Json::array() } } },
			};
			for (const auto& [method, params] : refused)
			{
				CAPTURE(method);
				const Json response = RequestAs(fixture, other, method, params);
				CHECK(GetErrorCode(response) == "InvalidState");
				CHECK(response["error"]["data"]["detail"].dump().contains("client 'test'"));
			}
			// Reads stay open to every client, and the owner is named.
			const Json state = RequestAs(fixture, other, "play.state", Json::object());
			CHECK(state["result"]["lockstepOwner"] == Json("test"));
			CHECK(state["result"]["ownedByCaller"] == Json(false));
			CHECK(state["result"]["tick"] == Json(0));
			const Result<Json> owner = fixture.Call("play.state", Json::object());
			REQUIRE(owner.has_value());
			CHECK((*owner)["ownedByCaller"] == Json(true));
			CHECK((*owner)["timeScale"] == Json(1.0));
		}

		TEST_CASE("PlayMethods: one play.step runs at a time, and a play.step whose session ends resolves with Cancelled")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			AutomationServer& server = fixture.GetClient().GetServer();
			REQUIRE(fixture.Call("play.start", Json{ { "paused", true } }).has_value());
			// Far more ticks than one frame's budget runs, so the step is still pending after a few frames.
			const int64_t step = fixture.GetClient().Submit("play.step", Json{ { "ticks", MaxPlayStepTicks }, { "render", "none" } });
			server.Pump();
			server.Pump();
			CHECK(server.TakeInProcessResponses(fixture.GetClient().GetClient()).empty());
			PlaySession* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			CHECK(session->IsStepping());
			CHECK(fixture.GetEditor().GetPlay().IsFrameThrottleSuspended());
			CHECK(session->GetTick() > 0);

			const ClientId other = server.ConnectInProcess("other");
			CHECK(GetErrorCode(RequestAs(fixture, other, "play.step", Json{ { "ticks", 1 } })) == "InvalidState");
			CHECK(GetErrorCode(RequestAs(fixture, other, "play.resume", Json::object())) == "InvalidState");
			const Json stopped = RequestAs(fixture, other, "play.stop", Json::object());
			REQUIRE(stopped.contains("result"));

			// The step's next poll finds its session gone.
			Json response;
			for (int pump = 0; pump < 10 && response.is_null(); ++pump)
			{
				server.Pump();
				for (Json& message : server.TakeInProcessResponses(fixture.GetClient().GetClient()))
				{
					if (message["id"] == Json(step))
						response = std::move(message);
				}
			}
			CHECK(GetErrorCode(response) == "Cancelled");
			CHECK(response["error"]["data"]["detail"].dump().contains("ended after"));
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsFrameThrottleSuspended());
		}

		TEST_CASE("PlayMethods: a play.step frame runs the ticks that fit 50 ms of the host's wall clock, and at least one")
		{
			// §4.2, §13.6: the budget splits the ticks across frames (play.step's "frames"); "every" runs one tick per frame.
			ScriptedWallClock clock;
			ScriptedClockSetup setup("PlayStepBudget", clock);
			REQUIRE(setup.GetClient().Call("play.start", Json{ { "lockstep", true } }).has_value());

			// 10 ms per reading: the frame's start, then one reading after each tick; the fifth tick's reading is 50 ms after
			// the start, which ends the frame. 12 ticks take 5 + 5 + 2.
			clock.Step = std::chrono::milliseconds(10);
			const Json budgeted = setup.Step(12, "none");
			CHECK(budgeted["ticks"] == Json(12));
			CHECK(budgeted["tick"] == Json(12));
			CHECK(budgeted["frames"] == Json(3));
			CHECK(budgeted["rendered"] == Json(0));

			// A tick that alone overruns the budget still runs: one per frame.
			clock.Step = std::chrono::milliseconds(100);
			const Json overrun = setup.Step(4, "last");
			CHECK(overrun["frames"] == Json(4));
			CHECK(overrun["rendered"] == Json(1));

			// A clock that does not move: every tick fits the first frame.
			clock.Step = std::chrono::milliseconds(0);
			const Json unbounded = setup.Step(50, "none");
			CHECK(unbounded["frames"] == Json(1));

			// "every" renders each tick in a frame of its own, whatever the budget allows.
			const Json every = setup.Step(3, "every");
			CHECK(every["frames"] == Json(3));
			CHECK(every["rendered"] == Json(3));
			CHECK(every["tick"] == Json(12 + 4 + 50 + 3));
		}

		TEST_CASE("PlayMethods: a pending play.step whose session another replaced at the same tick resolves with Cancelled")
		{
			// The step recognizes its session by PlaySession::GetSerial, never by its address, which the new session may reuse.
			ScriptedWallClock clock;
			ScriptedClockSetup setup("PlayStepReplaced", clock);
			AutomationServer& server = setup.GetClient().GetServer();
			const ClientId owner = setup.GetClient().GetClient();
			const ClientId other = server.ConnectInProcess("other");
			REQUIRE(setup.GetClient().Call("play.start", Json{ { "paused", true } }).has_value());
			const uint64_t firstSerial = setup.GetEditor().GetPlay().GetSession()->GetSerial();

			// The owner's step renders every tick, so each frame runs one; the other client's runs two per frame (25 ms per
			// reading).
			clock.Step = std::chrono::milliseconds(25);
			const int64_t ownerStep = setup.GetClient().Submit("play.step", Json{ { "ticks", 1000 }, { "render", "every" } });
			server.Pump(); // tick 1 of the first session
			// In one pump, after the owner's step has run tick 2: the other client stops the session, starts a new one and steps
			// it two ticks, to the tick the owner's step expects next.
			SubmitAs(server, other, 9001, "play.stop", Json::object());
			SubmitAs(server, other, 9002, "play.start", Json{ { "paused", true } });
			const int64_t otherStep = SubmitAs(server, other, 9003, "play.step", Json{ { "ticks", 6 }, { "render", "none" } });
			server.Pump();
			PlaySession* replacement = setup.GetEditor().GetPlay().GetSession();
			REQUIRE(replacement != nullptr);
			CHECK(replacement->GetSerial() != firstSerial);
			CHECK(replacement->GetTick() == 2);
			CHECK(replacement->IsStepping());

			Json ownerResponse;
			Json otherResponse;
			for (int pump = 0; pump < 20 && (ownerResponse.is_null() || otherResponse.is_null()); ++pump)
			{
				server.Pump();
				for (Json& message : server.TakeInProcessResponses(owner))
				{
					if (message["id"] == Json(ownerStep))
						ownerResponse = std::move(message);
				}
				for (Json& message : server.TakeInProcessResponses(other))
				{
					if (message["id"] == Json(otherStep))
						otherResponse = std::move(message);
				}
			}
			// The owner's step stopped at its session's end, and never ticked the new session: the other client's step ran
			// its six ticks undisturbed.
			CHECK(GetErrorCode(ownerResponse) == "Cancelled");
			CHECK(ownerResponse["error"]["data"]["detail"].dump().contains("ended after 2 of 1000 ticks"));
			REQUIRE_MESSAGE(otherResponse.contains("result"), otherResponse.dump());
			CHECK(otherResponse["result"]["tick"] == Json(6));
			CHECK(otherResponse["result"]["ticks"] == Json(6));
		}

		TEST_CASE("PlayMethods: an unexpected tick cancels the pending step and releases extraction immediately")
		{
			ScriptedWallClock clock{ .Step = std::chrono::milliseconds(100) };
			ScriptedClockSetup setup("StepUnexpectedTick", clock);
			Test::AutomationTestClient& client = setup.GetClient();
			REQUIRE(client.Call("play.start", Json{ { "paused", true } }).has_value());
			const int64_t request = client.Submit("play.step", Json{ { "ticks", 1000 }, { "render", "none" } });
			client.GetServer().Pump();
			PlaySession* session = setup.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			REQUIRE(session->IsStepping());
			session->Tick();
			const uint64_t tick = session->GetTick();
			client.GetServer().Pump();
			const std::vector<Json> responses = client.GetServer().TakeInProcessResponses(client.GetClient());
			REQUIRE(responses.size() == 1);
			CHECK(responses[0]["id"] == Json(request));
			CHECK(GetErrorCode(responses[0]) == "Cancelled");
			CHECK_FALSE(session->IsStepping());
			CHECK(session->GetTick() == tick);
			CHECK(session->IsPaused());
			const uint64_t before = session->GetExtractionCount();
			session->Tick();
			CHECK(session->GetExtractionCount() == before + 1);
		}

		TEST_CASE("PlayMethods: play.step input lands on the tick its offset names, and offsets beyond the step are refused")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Json space = Json{ { "tick", 2 }, { "type", "key" }, { "key", "Space" }, { "state", "down" } };
			const Json beyond = fixture.Request("play.step", Json{ { "ticks", 2 }, { "input", Json::array({ space }) } });
			CHECK(beyond["error"]["code"] == Json(-32602));
			CHECK(beyond["error"]["data"]["issues"][0]["pointer"] == Json("/input/0/tick"));
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetTick() == 0);

			const Result<Json> stepped = fixture.Call("play.step", Json{ { "ticks", 3 }, { "input", Json::array({ space }) }, { "render", "every" } });
			REQUIRE_MESSAGE(stepped.has_value(), stepped.error().ToString());
			CHECK((*stepped)["frames"] == Json(3));
			CHECK((*stepped)["rendered"] == Json(3));
			Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["input"]["pressed"] == Json::array({ "Key.Space" }));
			CHECK((*state)["input"]["down"] == Json::array({ "Key.Space" }));

			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["input"]["pressed"] == Json::array());
			CHECK((*state)["input"]["down"] == Json::array({ "Key.Space" }));
		}

		TEST_CASE("PlayMethods: the owner's pause leaves lockstep, resume refuses lockstep, and every change is a PlayStateChanged event")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			const uint64_t cursor = fixture.GetEditorFixture().GetEngine().GetEventLog().GetNextSeq();
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Result<Json> resumeLockstep = fixture.Call("play.resume", Json::object());
			REQUIRE_FALSE(resumeLockstep.has_value());
			CHECK(resumeLockstep.error().GetCode() == ErrorCode::InvalidState);

			const Result<Json> paused = fixture.Call("play.pause", Json::object());
			REQUIRE(paused.has_value());
			CHECK((*paused)["state"] == Json("Paused"));
			CHECK((*paused)["lockstep"] == Json(false));
			const Result<Json> resumed = fixture.Call("play.resume", Json::object());
			REQUIRE(resumed.has_value());
			CHECK((*resumed)["state"] == Json("Play"));
			const Result<Json> scaled = fixture.Call("play.setTimeScale", Json{ { "scale", 0.0 } });
			REQUIRE(scaled.has_value());
			CHECK((*scaled)["timeScale"] == Json(0.0));
			const Result<Json> stopped = fixture.Call("play.stop", Json::object());
			REQUIRE(stopped.has_value());
			CHECK((*stopped)["tick"] == Json(0));
			CHECK(JsonReader((*stopped)["stateHash"]).ReadString().value_or(std::string()).size() == 16);

			CHECK(ReadPlayStateEvents(fixture, cursor) == std::vector<std::string>{ "Play", "Paused", "Play", "Edit" });
			const Result<Json> notPlaying = fixture.Call("play.pause", Json::object());
			REQUIRE_FALSE(notPlaying.has_value());
			CHECK(notPlaying.error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("PlayMethods: play.start plays a named scene in the requested state and refuses a second start and bad paths")
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			REQUIRE(fixture.Call("scene.save", Json::object()).has_value());
			const Result<Json> started = fixture.Call("play.start",
				Json{ { "scene", "Assets/Scenes/Main.scene" }, { "mode", "simulate" }, { "paused", true }, { "timeScale", 2.0 }, { "seed", 9 } });
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			CHECK((*started)["state"] == Json("Paused"));
			CHECK((*started)["mode"] == Json("Simulate"));
			CHECK((*started)["timeScale"] == Json(2.0));
			CHECK((*started)["entityCount"] == Json(1));
			CHECK((*started)["maxEntities"] == Json(65536));
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetSeed() == 9);
			CHECK(fixture.GetEditor().GetPlay().GetFrameTimeScale() == 2.0);

			const Result<Json> again = fixture.Call("play.start", Json::object());
			REQUIRE_FALSE(again.has_value());
			CHECK(again.error().GetCode() == ErrorCode::InvalidState);
			REQUIRE(fixture.Call("play.stop", Json::object()).has_value());

			const Json notAScene = fixture.Request("play.start", Json{ { "scene", "Assets/Notes.txt" } });
			CHECK(notAScene["error"]["data"]["issues"][0]["pointer"] == Json("/scene"));
			const Result<Json> missing = fixture.Call("play.start", Json{ { "scene", "Assets/Scenes/Missing.scene" } });
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsPlaying());
		}
	}

}
