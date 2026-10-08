#include "TestsPCH.h"

#include "Engine/Session/PlayInput.h"

#include "Engine/Project/ProjectSettings.h"

#include <string>
#include <vector>

// PlayInput (Architecture §4.3, §5.7 step 1, §13.6). Skipped skeletons of the M7 contract
// (Docs/Decisions/0012-m7-decisions.md decision 4): stream A implements PlayInput and removes the skips.

namespace Engine {

	namespace {

		// Jump (Button: Key.Space) and MoveX (Axis: D positive, A negative).
		InputSettings MakeActions()
		{
			InputSettings settings;
			InputActionSettings jump;
			jump.Type = InputActionSettings::ActionType::Button;
			jump.Bindings = { "Key.Space" };
			settings.Actions.emplace("Jump", jump);
			InputActionSettings moveX;
			moveX.Type = InputActionSettings::ActionType::Axis;
			moveX.Positive = { "Key.D" };
			moveX.Negative = { "Key.A" };
			settings.Actions.emplace("MoveX", moveX);
			return settings;
		}

		PlayInputEvent MakeActionEvent(std::string name, PlayInputEventState state)
		{
			PlayInputEvent event;
			event.Type = PlayInputEventType::Action;
			event.Name = std::move(name);
			event.State = state;
			return event;
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlayInput: a tap reports pressed and released once each, on consecutive ticks" * doctest::skip(true))
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const uint32_t jump = *input.GetActions().FindAction("Jump");

			REQUIRE(input.Queue(0, MakeActionEvent("Jump", PlayInputEventState::Tap)).has_value());
			input.ApplyTick(0);
			CHECK(input.WasActionPressed(InputPhase::Step, jump));
			CHECK(input.IsActionDown(InputPhase::Step, jump));
			CHECK_FALSE(input.WasActionReleased(InputPhase::Step, jump));
			CHECK(input.GetSummary(InputPhase::Step).Pressed == std::vector<std::string>{ "Action.Jump" });

			input.ApplyTick(1);
			CHECK_FALSE(input.WasActionPressed(InputPhase::Step, jump));
			CHECK(input.WasActionReleased(InputPhase::Step, jump));
			CHECK_FALSE(input.IsActionDown(InputPhase::Step, jump));

			input.ApplyTick(2);
			CHECK_FALSE(input.WasActionPressed(InputPhase::Step, jump));
			CHECK_FALSE(input.WasActionReleased(InputPhase::Step, jump));
			CHECK(input.GetSummary(InputPhase::Step) == PlayInputSummary{});
		}

		TEST_CASE("PlayInput: injected actions combine with their bindings" * doctest::skip(true))
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const uint32_t moveX = *input.GetActions().FindAction("MoveX");

			PlayInputEvent axis;
			axis.Type = PlayInputEventType::Action;
			axis.Name = "MoveX";
			axis.HasValue = true;
			axis.Value = 0.35f;
			REQUIRE(input.Queue(0, axis).has_value());
			PlayInputEvent key;
			key.Type = PlayInputEventType::KeyInput;
			key.KeyCode = Key::A;
			REQUIRE(input.Queue(0, key).has_value());
			input.ApplyTick(0);
			// The binding gives -1, the injected value 0.35: the larger magnitude wins.
			CHECK(input.GetActionAxis(InputPhase::Step, moveX) == doctest::Approx(-1.0f));
			CHECK(input.GetLastAppliedEvents().size() == 2);
		}

		TEST_CASE("PlayInput: events for a tick that already ran and invalid events are rejected" * doctest::skip(true))
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			input.ApplyTick(0);
			const Status late = input.Queue(0, MakeActionEvent("Jump", PlayInputEventState::Down));
			REQUIRE_FALSE(late.has_value());
			CHECK(late.error().GetCode() == ErrorCode::InvalidArgument);

			const Status unknown = input.Queue(1, MakeActionEvent("Jmup", PlayInputEventState::Down));
			REQUIRE_FALSE(unknown.has_value());
			CHECK(unknown.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(unknown.error().GetHint().find("Jump") != std::string::npos);
			CHECK(input.GetQueuedEventCount() == 0);
		}

		TEST_CASE("PlayInput: event types and states have their registry names")
		{
			// Implemented by the contract: the names of the registry enums "InputEventType" and "InputEventState".
			CHECK(PlayInputEventTypeToString(PlayInputEventType::Action) == "Action");
			CHECK(PlayInputEventTypeToString(PlayInputEventType::KeyInput) == "Key");
			CHECK(PlayInputEventTypeToString(PlayInputEventType::GamepadAxisInput) == "GamepadAxis");
			CHECK(PlayInputEventStateToString(PlayInputEventState::Tap) == "Tap");
		}
	}

}
