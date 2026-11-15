#include "TestsPCH.h"

#include "Engine/Automation/Methods/ScriptMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"

#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <string>

namespace Engine {

	namespace {

		Json CallScriptMethod(Test::AutomationFixture& fixture, std::string_view method, const Json& params)
		{
			auto result = fixture.Call(method, params);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			return std::move(*result);
		}

		// Writes Assets/Scripts/Faulty.luau, a behaviour whose `callback` calls a local function that runs `raise` (an error
		// call), and attaches it to a new root entity "Board": the game's own script errors, which script.errors lists (a failed
		// script.eval is the caller's, ADR 0038 friction F01).
		void CreateFaultyBoard(Test::AutomationFixture& fixture, std::string_view callback, std::string_view raise)
		{
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Faulty.luau" }, { "source", std::format("local Faulty = {{}}\n"
																															  "local function fail() {} end\n"
																															  "function Faulty.{}(self: any, ...: any)\n"
																															  "\tfail()\n"
																															  "end\n"
																															  "return Script.Define('Faulty', Faulty)\n",
																													  raise, callback) } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Board" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Faulty.luau" } } }) } } } } } });
		}

	}

	TEST_SUITE("Automation")
	{
		// The host's error stream outlives each session: a session's errors stay listed after play.stop, and a cursor taken
		// before the next play.start reads that session's errors. Each session's occurrence starts its own count, since a new
		// session forgets the earlier ones' errors (ADR 0038, friction F02).
		TEST_CASE("ScriptMethods: host error cursors survive play session replacement")
		{
			Test::AutomationFixture fixture("ScriptHostHistory");
			CreateFaultyBoard(fixture, "OnStart", "error('persistent host fault', 0)");
			Json previous = "0";
			for (int occurrence = 1; occurrence <= 2; ++occurrence)
			{
				{
					Test::ExpectLog expected(LogLevel::Error, "persistent host fault");
					CallScriptMethod(fixture, "play.start", Json{ { "paused", true } });
				}
				CallScriptMethod(fixture, "play.stop", Json::object());
				const Json errors = CallScriptMethod(fixture, "script.errors", Json{ { "since", previous } });
				REQUIRE(errors["errors"].size() == 1);
				CHECK(errors["errors"][0]["count"] == Json(1));
				CHECK(errors["nextCursor"] != previous);
				previous = errors["nextCursor"];
			}
		}

		TEST_CASE("ScriptMethods: edit evaluation returns a detached value and captured prints")
		{
			Test::AutomationFixture fixture("ScriptEvalValue");
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "edit" }, { "code", "print('first'); print('second'); return { answer = 42, ready = true }" } });
			CHECK(result["value"] == Json({ { "answer", 42 }, { "ready", true } }));
			CHECK(result["prints"] == Json::array({ "first", "second" }));
		}

		TEST_CASE("ScriptMethods: edit evaluation refuses host writes before changing the scene")
		{
			Test::AutomationFixture fixture("ScriptEvalReadOnly");
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Before" } });
			const Json before = CallScriptMethod(fixture, "entity.get", Json{ { "entity", "/Before" } });
			{
				Test::ExpectLog expected(LogLevel::Error, "read-only");
				const Json response = fixture.Request("script.eval", Json{ { "context", "edit" }, { "code", "Scene.FindByName('Before').Name = 'After'" } });
				CHECK(response["error"]["data"]["errorCode"] == Json("Script"));
			}
			const Json after = CallScriptMethod(fixture, "entity.get", Json{ { "entity", "/Before" } });
			CHECK(after["entity"] == before["entity"]);
			CHECK(after["entity"]["components"] == before["entity"]["components"]);
			CHECK(fixture.Call("entity.get", Json{ { "entity", "/Before" } }).has_value());
			CHECK_FALSE(fixture.Call("entity.get", Json{ { "entity", "/After" } }).has_value());
		}

		TEST_CASE("ScriptMethods: eval requires code and context and rejects unknown enum values")
		{
			Test::AutomationFixture fixture("ScriptEvalParams");
			for (const Json& params : { Json{ { "code", "return 1" } }, Json{ { "context", "edit" } },
					 Json{ { "code", "return 1" }, { "context", "native" } } })
			{
				const Json response = fixture.Request("script.eval", params);
				CHECK(response["error"]["code"] == Json(-32602));
			}
		}

		TEST_CASE("ScriptMethods: play evaluation requires a live scripting session")
		{
			Test::AutomationFixture fixture("ScriptEvalPlayState");
			const Json response = fixture.Request("script.eval", Json{ { "context", "play" }, { "code", "return 1" } });
			CHECK(response["error"]["data"]["errorCode"] == Json("InvalidState"));
		}

		TEST_CASE("ScriptMethods: eval binds self to the selected behaviour instance")
		{
			Test::AutomationFixture fixture("ScriptEvalSelf");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/State.luau" }, { "source", "local State = {}; function State.OnCreate(self: any) self.Score = 7 end; return Script.Define('State', State)" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/State.luau" } } }) } } } } } });
			CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
			const Json result = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "play" }, { "entity", "/Game" }, { "code", "return self.Score" } });
			CHECK(result["value"] == Json(7));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// Docs/Decisions/0031-m20-contract.md decision 18: script names the behaviour whose slot self binds to, in both
		// contexts; a name no slot of the entity has is NotFound at /script before anything runs, and script needs entity.
		// The entities with several slots are ScriptSlotTests' and test_script_slots.py's.
		TEST_CASE("ScriptMethods: eval binds self to the named behaviour's slot and locates a name no slot has")
		{
			Test::AutomationFixture fixture("ScriptEvalBehaviour");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/State.luau" }, { "source", "local State = {}; function State.OnCreate(self: any) self.Score = 7 end; return Script.Define('State', State)" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/State.luau" } } }) } } } } } });
			const auto checkRefusals = [&fixture](std::string_view context)
			{
				CAPTURE(context);
				const Json missing = fixture.Request("script.eval", Json{ { "context", context }, { "entity", "/Game" }, { "script", "Missing" }, { "code", "Scene.CreateEntity('Ran') return 1" } });
				CHECK(missing["error"]["data"]["errorCode"] == Json("NotFound"));
				CHECK(missing["error"]["data"]["issues"][0]["pointer"] == Json("/script"));
				// An entity that does not resolve stays located at /entity.
				const Json noEntity = fixture.Request("script.eval", Json{ { "context", context }, { "entity", "/Nobody" }, { "script", "State" }, { "code", "return 1" } });
				CHECK(noEntity["error"]["data"]["errorCode"] == Json("NotFound"));
				CHECK(noEntity["error"]["data"]["issues"][0]["pointer"] == Json("/entity"));
				for (const Json& params : { Json{ { "context", context }, { "script", "State" }, { "code", "return 1" } },
						 Json{ { "context", context }, { "entity", "/Game" }, { "script", "" }, { "code", "return 1" } } })
				{
					const Json refused = fixture.Request("script.eval", params);
					CHECK(refused["error"]["data"]["errorCode"] == Json("InvalidArgument"));
					CHECK(refused["error"]["data"]["issues"][0]["pointer"] == Json("/script"));
				}
			};
			// Edit: the read-only VM builds the slot's table from its fields without running OnCreate.
			CHECK(CallScriptMethod(fixture, "script.eval", Json{ { "context", "edit" }, { "entity", "/Game" }, { "script", "State" }, { "code", "return self ~= nil and self.Entity.Name" } })["value"] == Json("Game"));
			checkRefusals("edit");
			CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
			CHECK(CallScriptMethod(fixture, "script.eval", Json{ { "context", "play" }, { "entity", "/Game" }, { "script", "State" }, { "code", "return self.Score" } })["value"] == Json(7));
			checkRefusals("play");
			CHECK(CallScriptMethod(fixture, "scene.query", Json{ { "where", Json{ { "name", "Ran" } } }, { "target", "play" } })["total"] == Json(0));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		TEST_CASE("ScriptMethods: errors expose locations and tracebacks and advance an exclusive cursor")
		{
			Test::AutomationFixture fixture("ScriptErrorCursor");
			CreateFaultyBoard(fixture, "OnStart", "error('contract error')");
			const auto eventCursor = fixture.GetEditor().GetEngine().GetEventLog().GetNextSeq();
			const Json start = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } });
			REQUIRE(start["errors"].empty());
			const std::string cursor = JsonReader(start["nextCursor"]).ReadString().value_or(std::string());
			{
				Test::ExpectLog expected(LogLevel::Error, "contract error");
				CallScriptMethod(fixture, "play.start", Json{ { "paused", true } });
				CHECK(expected.GetMatchCount() == 1);
			}
			const Json errors = CallScriptMethod(fixture, "script.errors", Json{ { "since", cursor }, { "limit", 1 } });
			REQUIRE(errors["errors"].size() == 1);
			const Json& error = errors["errors"][0];
			CHECK(error["kind"] == Json("runtime"));
			CHECK(error["line"] != Json(0));
			CHECK_FALSE(error["traceback"].empty());
			CHECK(error["count"] == Json(1));
			const auto raised = fixture.GetEditor().GetEngine().GetEventLog().Read(eventCursor, std::array{ EngineEventType::ScriptErrorRaised });
			REQUIRE(raised.Events.size() == 1);
			CHECK(Json(raised.Events[0].Message) == error["message"]);
			const Json next = CallScriptMethod(fixture, "script.errors", Json{ { "since", errors["nextCursor"] } });
			CHECK(next["errors"].empty());
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// The entity summary's path is "the entity's path from the scene root", which script.errors left empty. The stream
		// outlives the play session, so the path is read when the errors are: from the play scene while playing and from the
		// edit scene after play, and it is empty once the scene that reads address no longer holds the entity.
		TEST_CASE("ScriptMethods: errors give the entity's path in the scene that reads address while it holds the entity")
		{
			Test::AutomationFixture fixture("ScriptErrorEntityPath");
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Faulty.luau" }, { "source", "local Faulty = {}\n"
																												  "function Faulty.OnStart(self: any)\n"
																												  "\terror('board fault', 0)\n"
																												  "end\n"
																												  "return Script.Define('Faulty', Faulty)\n" } });
			CallScriptMethod(fixture, "entity.create", Json{ { "name", "Game" } });
			const Json board = CallScriptMethod(fixture, "entity.create", Json{ { "name", "Board" }, { "parent", "/Game" }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", "Assets/Scripts/Faulty.luau" } } }) } } } } } });
			const Json id = board["entity"]["id"];
			const auto readEntity = [&fixture]()
			{
				const Json errors = CallScriptMethod(fixture, "script.errors", Json::object());
				REQUIRE(errors["errors"].size() == 1);
				return errors["errors"][0]["entity"];
			};

			{
				// OnStart runs as the session starts.
				Test::ExpectLog expected(LogLevel::Error, "board fault");
				CallScriptMethod(fixture, "play.start", Json{ { "mode", "play" }, { "lockstep", true } });
				CHECK(expected.GetMatchCount() == 1);
			}
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "/Game/Board" } });
			// The entry names the faulting behaviour's slot (Docs/Decisions/0031-m20-contract.md decision 17).
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"][0]["slot"] == Json(0));
			// Destroyed in the play scene: the id and name stay, the path is empty.
			CallScriptMethod(fixture, "entity.destroy", Json{ { "entities", Json::array({ "/Game/Board" }) }, { "target", "play" } });
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "" } });
			// After play the edit scene, which still holds it, gives the path; destroyed there too, it is empty again.
			CallScriptMethod(fixture, "play.stop", Json::object());
			CHECK(readEntity()["path"] == Json("/Game/Board"));
			CallScriptMethod(fixture, "entity.destroy", Json{ { "entities", Json::array({ "/Game/Board" }) } });
			CHECK(readEntity() == Json{ { "id", id }, { "name", "Board" }, { "path", "" } });
		}

		TEST_CASE("ScriptMethods: errors preserve distinct embedded expectation pointers across RPC cursor pages")
		{
			Test::AutomationFixture fixture("ScriptEmbeddedErrorCursor");
			CallScriptMethod(fixture, "input.record", Json{ { "action", "start" } });
			const std::string replayPath = "Assets/Tests/EmbeddedErrors.replay";
			CallScriptMethod(fixture, "input.record", Json{ { "action", "stop" }, { "path", replayPath } });
			// input.record's Stop verifies every expectation before it writes a recording (Docs/Decisions/0030-m19-contract.md
			// decision 4), so the replay gets its failing expectations through the project's file system.
			const auto file = VfsPath::Create("project", replayPath);
			REQUIRE(file.has_value());
			const auto source = fixture.GetEditor().GetVfs().ReadText(*file);
			REQUIRE(source.has_value());
			ReplayLoadReport report;
			Result<ReplayDocument> replay = ReplayFromText(*source, report, true);
			REQUIRE_MESSAGE(replay.has_value(), (replay ? "" : replay.error().ToString()));
			const ReplayExpectation expectation{ .Tick = 0, .Luau = "return error('embedded expectation error', 0)" };
			replay->Expect = { expectation, expectation };
			const Result<std::string> text = ReplayToText(*replay);
			REQUIRE_MESSAGE(text.has_value(), (text ? "" : text.error().ToString()));
			REQUIRE(fixture.GetEditor().GetVfs().WriteFileAtomic(*file, AsBytes(*text)).has_value());
			CallScriptMethod(fixture, "asset.reimport", Json{ { "asset", replayPath } });
			const Json start = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } });
			REQUIRE(start["errors"].empty());
			{
				Test::ExpectLog expected(LogLevel::Error, "embedded expectation error");
				const Json response = fixture.Request("input.replay", Json{ { "path", replayPath }, { "verify", true } });
				CHECK(response["error"]["data"]["errorCode"] == Json("Validation"));
			}
			const Json first = CallScriptMethod(fixture, "script.errors", Json{ { "since", start["nextCursor"] }, { "limit", 1 } });
			REQUIRE(first["errors"].size() == 1);
			const Json second = CallScriptMethod(fixture, "script.errors", Json{ { "since", first["nextCursor"] }, { "limit", 1 } });
			REQUIRE(second["errors"].size() == 1);
			CHECK(first["errors"][0]["jsonPointer"] == Json("/Expect/0/Luau"));
			CHECK(second["errors"][0]["jsonPointer"] == Json("/Expect/1/Luau"));
			CHECK(first["errors"][0]["message"] == Json("/Expect/0/Luau: embedded expectation error"));
			CHECK(second["errors"][0]["message"] == Json("/Expect/1/Luau: embedded expectation error"));
			CHECK(first["nextCursor"] != second["nextCursor"]);
			for (const Json& page : { first, second })
			{
				CHECK(page["errors"][0]["script"] == Json(replayPath));
				CHECK(page["errors"][0]["line"] == Json(1));
				CHECK(page["errors"][0]["count"] == Json(1));
			}
			const Json after = CallScriptMethod(fixture, "script.errors", Json{ { "since", second["nextCursor"] } });
			CHECK(after["errors"].empty());
		}

		// ADR 0038, friction F01: a failed script.eval is the caller's error. It is answered with ScriptError and its located
		// error in the response's scriptError data, and the game's script errors (script.errors) never list it, in either
		// context; its value holds vectors, Quats and Colors as arrays.
		TEST_CASE("ScriptMethods: a failed script.eval answers its caller and is not among the game's script errors")
		{
			Test::AutomationFixture fixture("ScriptEvalCallerError");
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true } });
			const auto eventCursor = fixture.GetEditor().GetEngine().GetEventLog().GetNextSeq();
			for (const std::string_view context : { "play", "edit" })
			{
				CAPTURE(context);
				Test::ExpectLog expected(LogLevel::Error, "caller fault");
				const Json response = fixture.Request("script.eval",
					Json{ { "context", context }, { "code", "local function fail() error('caller fault') end\nfail()" } });
				REQUIRE(response.contains("error"));
				const Json& data = response["error"]["data"];
				CHECK(data["errorCode"] == Json("Script"));
				REQUIRE_MESSAGE(data.contains("scriptError"), data.dump());
				CHECK(data["scriptError"]["kind"] == Json("runtime"));
				CHECK(data["scriptError"]["line"] == Json(1));
				CHECK(JsonReader(data["scriptError"]["message"]).ReadString().value_or(std::string()).contains("caller fault"));
				CHECK_FALSE(data["scriptError"]["traceback"].empty());
			}
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].empty());
			CHECK(fixture.GetEditor().GetEngine().GetEventLog().Read(eventCursor, std::array{ EngineEventType::ScriptErrorRaised }).Events.empty());
			const Json value = CallScriptMethod(fixture, "script.eval",
				Json{ { "context", "play" }, { "code", "return {Velocity = vector.create(1, 2, 3), Pose = {Rotation = Quat.Identity(), Tint = Color.New(1, 0, 0, 1)}}" } });
			CHECK(value["value"] == Json{ { "Pose", Json{ { "Rotation", Json::array({ 0, 0, 0, 1 }) }, { "Tint", Json::array({ 1, 0, 0, 1 }) } } }, { "Velocity", Json::array({ 1, 2, 3 }) } });
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// ADR 0038, friction F13: a failed script.eval's own failure is answered in its error data and is not one of the game's
		// script errors, so the response's "_meta" diagnostics neither count nor quote it, in either context, and agree with
		// script.errors; a script error the game raised since the client's previous response is still counted and quoted.
		TEST_CASE("ScriptMethods: a failed script.eval's own failure takes no part in _meta's diagnostics, the game's errors still do")
		{
			Test::AutomationFixture fixture("ScriptEvalMetaDiagnostics");
			CreateFaultyBoard(fixture, "OnFixedUpdate", "error('game fault', 0)");
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true }, { "pauseOnError", false } });
			const auto failEvaluation = [&fixture](std::string_view context)
			{
				const Json response = fixture.Request("script.eval",
					Json{ { "context", context }, { "code", "local function fail() error('caller fault') end\nfail()" } });
				REQUIRE(response.contains("error"));
				CHECK(response["error"]["data"]["errorCode"] == Json("Script"));
				return response["error"]["data"]["_meta"]["diagnostics"];
			};
			for (const std::string_view context : { "play", "edit" })
			{
				CAPTURE(context);
				Test::ExpectLog expected(LogLevel::Error, "caller fault");
				const Json diagnostics = failEvaluation(context);
				CHECK(diagnostics["newScriptErrors"] == Json(0));
				CHECK(diagnostics["newErrors"] == Json(0));
				CHECK(diagnostics["firstNew"] == Json::array());
			}
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].empty());

			// The game's fault, raised by a tick run between two requests (as live play runs frames between them), is in the
			// next response's diagnostics beside the evaluation's own failure, which still is not.
			{
				Test::ExpectLog expected(LogLevel::Error, "game fault");
				fixture.GetEditor().GetPlay().GetSession()->Tick();
			}
			Test::ExpectLog expected(LogLevel::Error, "caller fault");
			const Json diagnostics = failEvaluation("play");
			CHECK(diagnostics["newScriptErrors"] == Json(1));
			REQUIRE(diagnostics["firstNew"].size() == 1);
			CHECK(JsonReader(diagnostics["firstNew"][0]["message"]).ReadString().value_or(std::string()).contains("game fault"));
			CHECK(diagnostics["firstNew"][0]["logger"] == Json("Script"));
			REQUIRE(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].size() == 1);
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// ADR 0038, friction F16: a retained error's tick is the simulation tick its callback ran in, as Time.GetTick() reads it
		// there (in the fixed step the zero-based index of the tick being run, which is the ticks completed before it; in the
		// frame phase the ticks completed), and with pauseOnError false a faulting behaviour is disabled after its first error:
		// it runs no further callback, so it never raises the error again, while play and the other behaviours run on. count
		// counts the occurrences of the same error in the session, here one per faulting instance.
		TEST_CASE("ScriptMethods: script.errors reports the tick an error's callback ran in, and a faulting behaviour runs no further callback")
		{
			Test::AutomationFixture fixture("ScriptErrorTicks");
			// Ticks.luau: every instance counts its fixed steps in Steps, and `fault` adds the callbacks that raise.
			const auto writeTicks = [&fixture](std::string_view fault)
			{
				const std::string source = std::format("local Ticks = {{}}\n"
													   "function Ticks.OnCreate(self: any)\n"
													   "\tself.Steps = 0\n"
													   "end\n"
													   "function Ticks.OnFixedUpdate(self: any)\n"
													   "\tself.Steps += 1\n"
													   "end\n"
													   "{}\n"
													   "return Script.Define('Ticks', Ticks)\n",
					fault);
				CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Ticks.luau" }, { "source", source } });
			};
			const auto attach = [&fixture](std::string_view name, std::string_view script)
			{
				CallScriptMethod(fixture, "entity.create", Json{ { "name", name }, { "components", Json{ { "Script", Json{ { "Scripts", Json::array({ Json{ { "Script", script } } }) } } } } } });
			};
			const auto steps = [&fixture](std::string_view entity)
			{
				return CallScriptMethod(fixture, "script.eval", Json{ { "context", "play" }, { "entity", entity }, { "code", "return self.Steps" } })["value"];
			};
			const std::string counter = "local Counter = {}\n"
										"function Counter.OnCreate(self: any)\n"
										"\tself.Steps = 0\n"
										"end\n"
										"function Counter.OnFixedUpdate(self: any)\n"
										"\tself.Steps += 1\n"
										"end\n"
										"return Script.Define('Counter', Counter)\n";
			CallScriptMethod(fixture, "script.write", Json{ { "path", "Assets/Scripts/Counter.luau" }, { "source", counter } });
			attach("Counter", "Assets/Scripts/Counter.luau");

			// A frame-phase fault: the first OnLateUpdate runs after tick 0, when one tick has completed.
			writeTicks("function Ticks.OnLateUpdate(self: any)\n"
					   "\terror('late fault', 0)\n"
					   "end");
			attach("Faulty", "Assets/Scripts/Ticks.luau");
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true }, { "pauseOnError", false } });
			{
				Test::ExpectLog late(LogLevel::Error, "late fault");
				const Json stepped = CallScriptMethod(fixture, "play.step", Json{ { "ticks", 5 } });
				CHECK(stepped["_meta"]["tick"] == Json(5));
				CHECK(stepped["_meta"]["playState"] == Json("Play"));
				CHECK(late.GetMatchCount() == 1);
			}
			const Json lateErrors = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(lateErrors["errors"].size() == 1);
			CHECK(lateErrors["errors"][0]["callback"] == Json("OnLateUpdate"));
			CHECK(lateErrors["errors"][0]["tick"] == Json(1));
			CHECK(lateErrors["errors"][0]["count"] == Json(1));
			CHECK(steps("/Faulty") == Json(1)); // tick 0's OnFixedUpdate only
			CHECK(steps("/Counter") == Json(5));
			CallScriptMethod(fixture, "play.stop", Json::object());

			// A fixed-step fault from tick 2 on, in two instances: each raises it once, in the third tick being run (index 2),
			// and runs none of its callbacks after it (its OnUpdate would raise another error from the frame after tick 2).
			writeTicks("function Ticks.OnUpdate(self: any)\n"
					   "\tif Time.GetTick() >= 3 then error('frame fault', 0) end\n"
					   "end\n"
					   "local step = Ticks.OnFixedUpdate\n"
					   "function Ticks.OnFixedUpdate(self: any)\n"
					   "\tstep(self)\n"
					   "\tif Time.GetTick() >= 2 then error('fixed fault', 0) end\n"
					   "end");
			attach("Faulty2", "Assets/Scripts/Ticks.luau");
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true }, { "pauseOnError", false } });
			{
				Test::ExpectLog fixed(LogLevel::Error, "fixed fault");
				const Json stepped = CallScriptMethod(fixture, "play.step", Json{ { "ticks", 6 } });
				CHECK(stepped["_meta"]["tick"] == Json(6));
				CHECK(stepped["_meta"]["playState"] == Json("Play"));
				CHECK(fixed.GetMatchCount() == 2);
			}
			const Json fixedErrors = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(fixedErrors["errors"].size() == 1);
			CHECK(fixedErrors["errors"][0]["callback"] == Json("OnFixedUpdate"));
			CHECK(fixedErrors["errors"][0]["tick"] == Json(2));
			CHECK(fixedErrors["errors"][0]["count"] == Json(2));
			CHECK(steps("/Faulty") == Json(3)); // ticks 0, 1 and 2, the last one faulting
			CHECK(steps("/Faulty2") == Json(3));
			CHECK(steps("/Counter") == Json(6));
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		// ADR 0038, friction F02: script.errors reports the current play session's errors. The errors of an earlier session
		// stay listed after it stops, until a new session starts; a cursor taken before that start still reads exactly the new
		// session's errors, and an error the new session raises again starts a new count.
		TEST_CASE("ScriptMethods: script.errors reports the current play session's errors only")
		{
			Test::AutomationFixture fixture("ScriptErrorsPerSession");
			CreateFaultyBoard(fixture, "OnFixedUpdate", "error('session fault', 0)");
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true }, { "pauseOnError", false } });
			{
				Test::ExpectLog expected(LogLevel::Error, "session fault");
				CallScriptMethod(fixture, "play.step", Json{ { "ticks", 1 } });
			}
			CallScriptMethod(fixture, "play.stop", Json::object());
			const Json stopped = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(stopped["errors"].size() == 1);
			const Json cursor = CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } })["nextCursor"];
			CallScriptMethod(fixture, "play.start", Json{ { "lockstep", true }, { "pauseOnError", false } });
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].empty());
			CHECK(CallScriptMethod(fixture, "script.errors", Json{ { "since", cursor } })["errors"].empty());
			{
				Test::ExpectLog expected(LogLevel::Error, "session fault");
				CallScriptMethod(fixture, "play.step", Json{ { "ticks", 1 } });
			}
			const Json current = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(current["errors"].size() == 1);
			CHECK(current["errors"][0]["count"] == Json(1));
			CHECK(current["errors"][0]["tick"] == stopped["errors"][0]["tick"]);
			CHECK(CallScriptMethod(fixture, "script.errors", Json{ { "since", cursor } })["errors"] == current["errors"]);
			CallScriptMethod(fixture, "play.stop", Json::object());
			// A recording's session (input.record, as input.replay's) is a new play session too.
			REQUIRE(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].size() == 1);
			CallScriptMethod(fixture, "scene.save", Json::object());
			CallScriptMethod(fixture, "input.record", Json{ { "action", "start" } });
			CHECK(CallScriptMethod(fixture, "script.errors", Json::object())["errors"].empty());
			CallScriptMethod(fixture, "play.stop", Json::object());
		}

		TEST_CASE("ScriptMethods: errors rejects malformed cursors and limits with located errors")
		{
			Test::AutomationFixture fixture("ScriptErrorsInvalid");
			for (const Json& params : { Json{ { "since", "-1" } }, Json{ { "since", "18446744073709551616" } },
					 Json{ { "limit", 0 } }, Json{ { "limit", 1001 } } })
			{
				const Json response = fixture.Request("script.errors", params);
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK_FALSE(response["error"]["data"]["issues"].empty());
			}
		}

		TEST_CASE("ScriptMethods: eval refuses dry runs and atomic batches before execution")
		{
			Test::AutomationFixture fixture("ScriptEvalFlags");
			const Json params{ { "context", "edit" }, { "code", "return 1" }, { "dryRun", true } };
			CHECK(fixture.Request("script.eval", params)["error"]["data"]["errorCode"] == Json("Unsupported"));
			const Json batch = fixture.Request("edit.batch", Json{ { "ops", Json::array({ Json{ { "method", "script.eval" }, { "params", Json{ { "context", "edit" }, { "code", "return 1" } } } } }) } });
			CHECK(batch.contains("error"));
		}

		TEST_CASE("ScriptMethods: errors keep bounded messages and report evictions to older cursors")
		{
			Test::AutomationFixture fixture("ScriptErrorBounds");
			const std::string marker = " [truncated: 20000 bytes]";
			{
				// The caller's own error data is bounded as a retained error is.
				Test::ExpectLog expected(LogLevel::Error, marker);
				const Json response = fixture.Request("script.eval", Json{ { "context", "edit" }, { "code", "error(string.rep('x', 20000), 0)" } });
				REQUIRE(response.contains("error"));
				const Json& data = response["error"]["data"];
				CHECK(data["errorCode"] == Json("Script"));
				REQUIRE_MESSAGE(data.contains("scriptError"), data.dump().substr(0, 512));
				CHECK(data["scriptError"]["message"] == Json(std::string(MaxScriptLogMessageBytes - marker.size(), 'x') + marker));
				CHECK(expected.GetMatchCount() == 1);
			}
			CreateFaultyBoard(fixture, "OnStart", "error(string.rep('x', 20000), 0)");
			{
				Test::ExpectLog expected(LogLevel::Error, marker);
				CallScriptMethod(fixture, "play.start", Json{ { "paused", true } });
				CHECK(expected.GetMatchCount() == 1);
			}
			const Json first = CallScriptMethod(fixture, "script.errors", Json::object());
			REQUIRE(first["errors"].size() == 1);
			CHECK(first["errors"][0]["message"] == Json(std::string(MaxScriptLogMessageBytes - marker.size(), 'x') + marker));
			CHECK(first["evicted"] == Json(false));
			// The editor's history keeps the newest distinct errors: one more than it retains evicts the behaviour's error.
			ScriptErrorStream& history = fixture.GetEditor().GetPlay().GetScriptErrors();
			for (size_t index = 0; index < MaxScriptErrorEntries; ++index)
				static_cast<void>(history.Add({ .Script = "Assets/Scripts/Spam.luau", .Line = 1, .Message = std::format("fault {}", index) }));
			CHECK(history.GetErrors().size() == MaxScriptErrorEntries);
			CHECK(history.GetEvictedCount() == 1);
			// Pages stay small enough to be answered inline.
			const Json oldest = CallScriptMethod(fixture, "script.errors", Json{ { "limit", 1 } });
			CHECK(oldest["evicted"] == Json(true));
			REQUIRE(oldest["errors"].size() == 1);
			CHECK(oldest["errors"][0]["message"] == Json("fault 0"));
			// A reader that already saw the evicted error missed nothing; its next page starts at the oldest retained one.
			const Json seen = CallScriptMethod(fixture, "script.errors", Json{ { "since", first["nextCursor"] }, { "limit", 1 } });
			CHECK(seen["evicted"] == Json(false));
			REQUIRE(seen["errors"].size() == 1);
			CHECK(seen["errors"][0]["message"] == Json("fault 0"));
			CHECK(seen["nextCursor"] == oldest["nextCursor"]);
			const Json rest = CallScriptMethod(fixture, "script.errors", Json{ { "since", std::to_string(history.GetCursor()) } });
			CHECK(rest["evicted"] == Json(false));
			CHECK(rest["errors"].empty());
			CHECK(CallScriptMethod(fixture, "script.errors", Json{ { "since", "end" } })["evicted"] == Json(false));
		}
	}

}
