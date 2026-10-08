#include "TestsPCH.h"

#include "Engine/Automation/Methods/InputMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

#include <string>
#include <utility>
#include <vector>

// input.inject in process (Architecture §13.5, §13.6; Docs/Decisions/0012-m7-decisions.md decision 4).

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("InputMethods: input.inject queues events for the next tick and play.state shows them")
		{
			Test::AutomationFixture fixture("InputMethods");
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Result<Json> injected = fixture.Call("input.inject",
				Json{ { "events", Json::array({ Json{ { "type", "key" }, { "key", "Space" }, { "state", "tap" } } }) } });
			REQUIRE_MESSAGE(injected.has_value(), injected.error().ToString());
			CHECK((*injected)["tick"] == Json(0));
			CHECK((*injected)["queued"] == Json(1));

			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			const Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["input"]["pressed"] == Json::array({ "Key.Space" }));
		}

		TEST_CASE("InputMethods: input.inject validates every event before it queues any")
		{
			Test::AutomationFixture fixture("InputMethods");
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Json events = Json::array({
				Json{ { "type", "key" }, { "key", "Space" } },
				Json{ { "type", "key" }, { "key", "Spcae" } },
			});
			const Json response = fixture.Request("input.inject", Json{ { "events", events } });
			CHECK(response["error"]["code"] == Json(-32602));
			CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json("/events/1/key"));
			CHECK(response["error"]["data"]["issues"][0]["hint"].dump().contains("Space"));
			REQUIRE(fixture.GetEditor().GetPlay().GetSession() != nullptr);
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetInput().GetQueuedEventCount() == 0);
		}

		TEST_CASE("InputMethods: releaseAll releases what is held at the next tick, before the call's events")
		{
			Test::AutomationFixture fixture("InputMethods");
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const Json held = Json::array({ Json{ { "type", "key" }, { "key", "A" } }, Json{ { "type", "mouseButton" }, { "button", "Left" }, { "position", Json::array({ 4, 5 }) } },
				Json{ { "type", "gamepadAxis" }, { "gamepad", 1 }, { "axis", "LeftX" }, { "value", 0.5 } } });
			REQUIRE(fixture.Call("input.inject", Json{ { "events", held } }).has_value());
			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			Result<Json> state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["input"]["down"] == Json::array({ "Key.A", "Mouse.Left" }));
			CHECK((*state)["input"]["axes"]["Gamepad.LeftX"] == Json(0.5));

			// Release everything, then press B in the same tick: the release comes first.
			const Result<Json> released =
				fixture.Call("input.inject", Json{ { "events", Json::array({ Json{ { "type", "key" }, { "key", "B" } } }) }, { "releaseAll", true } });
			REQUIRE_MESSAGE(released.has_value(), released.error().ToString());
			CHECK((*released)["tick"] == Json(1));
			CHECK((*released)["queued"] == Json(1));
			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			state = fixture.Call("play.state", Json::object());
			REQUIRE(state.has_value());
			CHECK((*state)["input"]["down"] == Json::array({ "Key.B" }));
			CHECK((*state)["input"]["released"] == Json::array({ "Key.A", "Mouse.Left" }));
			CHECK((*state)["input"]["axes"] == Json::object());
		}

		TEST_CASE("InputMethods: each event type reads only its own members and needs its required ones")
		{
			Test::AutomationFixture fixture("InputMethods");
			REQUIRE(fixture.Call("play.start", Json{ { "lockstep", true } }).has_value());
			const std::vector<std::pair<Json, std::string>> invalid = {
				{ Json{ { "key", "Space" } }, "/events/0/type" },
				{ Json{ { "type", "key" }, { "key", "Space" }, { "text", "a" } }, "/events/0/text" },
				{ Json{ { "type", "mouseMove" } }, "/events/0/position" },
				{ Json{ { "type", "gamepadButton" }, { "button", "Southh" } }, "/events/0/button" },
				{ Json{ { "type", "gamepadAxis" }, { "axis", "LeftTrigger" }, { "value", -0.5 } }, "/events/0/value" },
				{ Json{ { "type", "text" }, { "text", "" } }, "/events/0/text" },
			};
			for (const auto& [event, pointer] : invalid)
			{
				CAPTURE(event.dump());
				const Json response = fixture.Request("input.inject", Json{ { "events", Json::array({ event }) } });
				CHECK(response["error"]["code"] == Json(-32602));
				CHECK(response["error"]["data"]["issues"][0]["pointer"] == Json(pointer));
			}
			const Json valid = Json::array({ Json{ { "type", "text" }, { "text", "h\xC3\xA9" } }, Json{ { "type", "scroll" }, { "delta", Json::array({ 0, 1 }) } },
				Json{ { "type", "mouseDelta" }, { "delta", Json::array({ 3, -2 }) } } });
			const Result<Json> injected = fixture.Call("input.inject", Json{ { "events", valid } });
			REQUIRE_MESSAGE(injected.has_value(), injected.error().ToString());
			CHECK((*injected)["queued"] == Json(3));
			REQUIRE(fixture.Call("play.step", Json{ { "ticks", 1 } }).has_value());
			const InputState& devices = fixture.GetEditor().GetPlay().GetSession()->GetInput().GetDevices();
			CHECK(devices.GetScrollDelta(InputPhase::Step).y == doctest::Approx(1.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Step).x == doctest::Approx(3.0f));
			CHECK(fixture.GetEditor().GetPlay().GetSession()->GetInput().GetLastAppliedEvents().size() == 3);
		}

		TEST_CASE("InputMethods: input.inject needs a play session")
		{
			Test::AutomationFixture fixture("InputMethods");
			const Result<Json> refused = fixture.Call("input.inject", Json{ { "events", Json::array() } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
