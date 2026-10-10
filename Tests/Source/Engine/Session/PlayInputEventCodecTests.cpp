#include "TestsPCH.h"
#include "Engine/Session/PlayInputEventCodec.h"

#include "Engine/Project/ProjectSettings.h"

#include <nlohmann/json.hpp>

#include <limits>

namespace Engine {

	namespace {

		PlayInput Input()
		{
			InputSettings settings;
			settings.Actions["Jump"] = {};
			settings.Actions["Move"].Type = InputActionSettings::ActionType::Axis;
			auto input = PlayInput::Create(settings);
			REQUIRE(input.has_value());
			return std::move(*input);
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlayInputEventCodec: automation and replay cover the same nine event kinds")
		{
			auto input = Input();
			const Json events = Json::array({ { { "type", "ACTION" }, { "name", "Jump" }, { "state", "tap" } },
				{ { "type", "Action" }, { "name", "Move" }, { "value", 0.5f } },
				{ { "type", "key" }, { "key", "space" } },
				{ { "type", "mouseButton" }, { "button", "left" }, { "position", { 2, 3 } } },
				{ { "type", "mouseMove" }, { "position", { 4, 5 } } },
				{ { "type", "mouseDelta" }, { "delta", { -1, 2 } } },
				{ { "type", "scroll" }, { "delta", { 0, 2 } } },
				{ { "type", "gamepadButton" }, { "gamepad", 3 }, { "button", "south" }, { "state", "Up" } },
				{ { "type", "gamepadAxis" }, { "gamepad", 2 }, { "axis", "LeftX" }, { "value", -0.25f } },
				{ { "type", "text" }, { "text", "snow â˜ƒ" } } });
			for (const auto& event : events)
			{
				auto parsed = ParsePlayInputEvent(event, input.GetActions());
				REQUIRE(parsed);
				auto encoded = EncodeReplayEvent(7, parsed->Event);
				REQUIRE(encoded);
				auto decoded = DecodeReplayEvent(*encoded, input.GetActions());
				REQUIRE(decoded);
				CHECK(decoded->Tick == 7);
				CHECK(decoded->Event == parsed->Event);
			}
		}

		TEST_CASE("PlayInputEventCodec: inappropriate members and nonfinite values have located errors")
		{
			auto input = Input();
			const Json events = Json::array({ { { "type", "key" }, { "key", "Space" }, { "value", 0 } },
				{ { "type", "action" }, { "name", "Jump" }, { "state", "down" }, { "value", 1 } },
				{ { "type", "mouseMove" }, { "position", { 1 } } },
				{ { "type", "gamepadAxis" }, { "axis", "LeftTrigger" }, { "value", -0.1f } },
				{ { "type", "gamepadButton" }, { "button", "South" }, { "gamepad", 4 } },
				{ { "type", "scroll" }, { "delta", { std::numeric_limits<double>::infinity(), 0 } } },
				{ { "type", "text" }, { "text", "" }, { "unexpected", true } } });
			for (const auto& event : events)
			{
				auto parsed = ParsePlayInputEvent(event, input.GetActions(), "/events/2");
				REQUIRE_FALSE(parsed);
				CHECK(parsed.error().GetCode() == ErrorCode::InvalidArgument);
				REQUIRE(parsed.error().GetLocation().JsonPointer);
				CHECK(parsed.error().GetLocation().JsonPointer->starts_with("/events/2"));
			}
		}

		TEST_CASE("PlayInputEventCodec: unknown actions and controls include suggestions")
		{
			auto input = Input();
			for (const auto& event : { Json{ { "type", "key" }, { "key", "Spac" } }, Json{ { "type", "action" }, { "name", "Jum" } } })
			{
				auto result = ParsePlayInputEvent(event, input.GetActions());
				REQUIRE_FALSE(result);
				CHECK_FALSE(result.error().GetHint().empty());
			}
		}

		TEST_CASE("PlayInputEventCodec: test injection refuses an explicit tick and queues nothing on error")
		{
			auto input = Input();
			auto result = ParsePlayInputEvent({ { "type", "key" }, { "key", "Space" }, { "tick", 0 } }, input.GetActions(), {}, false);
			REQUIRE_FALSE(result);
			CHECK(result.error().GetLocation().JsonPointer == "/tick");
			input.ApplyTick(0);
			CHECK(input.GetLastAppliedEvents().empty());
			CHECK_FALSE(input.GetDevices().IsKeyDown(InputPhase::Step, Key::Space));
		}

		TEST_CASE("PlayInputEventCodec: applied tap edges encode without a second expansion")
		{
			auto input = Input();
			auto parsed = ParsePlayInputEvent({ { "type", "key" }, { "key", "Space" }, { "state", "Tap" } }, input.GetActions());
			REQUIRE(parsed);
			REQUIRE(input.Queue(0, parsed->Event));
			for (uint64_t tick = 0; tick < 2; ++tick)
			{
				input.ApplyTick(tick);
				REQUIRE(input.GetLastAppliedEvents().size() == 1);
				auto encoded = EncodeReplayEvent(tick, input.GetLastAppliedEvents()[0]);
				REQUIRE(encoded);
				CHECK(encoded->State == (tick == 0 ? "Down" : "Up"));
			}
			input.ApplyTick(2);
			CHECK(input.GetLastAppliedEvents().empty());
		}
	}

}
