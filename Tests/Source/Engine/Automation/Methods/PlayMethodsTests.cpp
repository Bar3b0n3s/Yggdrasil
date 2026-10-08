#include "TestsPCH.h"

#include "Engine/Automation/Methods/PlayMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

// play.* in process, through the editor's server (Architecture §13.5, §13.6; ADR 0008: "an in-process round trip for
// every method"). Skipped skeletons of the M7 contract (Docs/Decisions/0012-m7-decisions.md decisions 3 and 7): stream A
// implements and registers the play methods and removes the skips.

namespace Engine {

	namespace {

		// One entity in the fixture's open scene, so play has something to copy.
		void OpenSceneWithEntity(Test::AutomationFixture& fixture)
		{
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Ball" } }).has_value());
		}

	}

	TEST_SUITE("Automation")
	{
		TEST_CASE("PlayMethods: play.start, play.state and play.stop report the session" * doctest::skip(true))
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

		TEST_CASE("PlayMethods: play.step runs its ticks and reports the tick and the state hash" * doctest::skip(true))
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

		TEST_CASE("PlayMethods: play.step needs lockstep or a paused session, and one at a time" * doctest::skip(true))
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

		TEST_CASE("PlayMethods: play.start refuses the members of later milestones at their pointer" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("PlayMethods");
			OpenSceneWithEntity(fixture);
			for (const char* member : { "parameters", "pauseOnError" })
			{
				Json params = Json::object();
				params[member] = std::string(member) == "parameters" ? Json::object() : Json(true);
				const Result<Json> refused = fixture.Call("play.start", params);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Unsupported);
				REQUIRE_FALSE(refused.error().GetIssues().empty());
				CHECK(refused.error().GetIssues().front().JsonPointer == std::string("/") + member);
			}
			CHECK_FALSE(fixture.GetEditor().GetPlay().IsPlaying());
		}

		TEST_CASE("PlayMethods: play.setTimeScale validates its range" * doctest::skip(true))
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

		TEST_CASE("PlayMethods: dryRun is Unsupported for every play method" * doctest::skip(true))
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
	}

}
