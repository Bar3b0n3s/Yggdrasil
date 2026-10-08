#include "TestsPCH.h"

#include "Engine/Automation/Methods/InputMethods.h"

#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Session/PlaySession.h"
#include "Support/AutomationTestClient.h"

#include <nlohmann/json.hpp>

// input.inject in process (Architecture §13.5, §13.6). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 4): stream A implements and registers input.inject and removes the skips.

namespace Engine {

	TEST_SUITE("Automation")
	{
		TEST_CASE("InputMethods: input.inject queues events for the next tick and play.state shows them" * doctest::skip(true))
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

		TEST_CASE("InputMethods: input.inject validates every event before it queues any" * doctest::skip(true))
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

		TEST_CASE("InputMethods: input.inject needs a play session" * doctest::skip(true))
		{
			Test::AutomationFixture fixture("InputMethods");
			const Result<Json> refused = fixture.Call("input.inject", Json{ { "events", Json::array() } });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
		}
	}

}
