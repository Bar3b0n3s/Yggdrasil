#include "TestsPCH.h"

#include "Engine/Session/PlayInput.h"

#include "Engine/Project/ProjectSettings.h"

#include <map>
#include <string>
#include <vector>

// PlayInput (Architecture §4.3, §5.7 step 1, §13.6; Docs/Decisions/0012-m7-decisions.md decision 4): the tick-stamped
// queue, taps, injected actions combined with bindings, and the views play.state reports.

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
		TEST_CASE("PlayInput: a tap reports pressed and released once each, on consecutive ticks")
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

		TEST_CASE("PlayInput: injected actions combine with their bindings")
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

		TEST_CASE("PlayInput: events for a tick that already ran and invalid events are rejected")
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

		TEST_CASE("PlayInput: the summary names controls and actions by binding, sorted, with the non-zero axes")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			PlayInputEvent key;
			key.Type = PlayInputEventType::KeyInput;
			key.KeyCode = Key::D;
			REQUIRE(input.Queue(0, key).has_value());
			PlayInputEvent space = key;
			space.KeyCode = Key::Space;
			REQUIRE(input.Queue(0, space).has_value());
			PlayInputEvent stick;
			stick.Type = PlayInputEventType::GamepadAxisInput;
			stick.Gamepad = 2;
			stick.Axis = GamepadAxis::LeftY;
			stick.Value = -0.5f;
			REQUIRE(input.Queue(0, stick).has_value());
			input.ApplyTick(0);

			const PlayInputSummary summary = input.GetSummary(InputPhase::Step);
			CHECK(summary.Down == std::vector<std::string>{ "Action.Jump", "Action.MoveX", "Key.D", "Key.Space" });
			CHECK(summary.Pressed == summary.Down);
			CHECK(summary.Released.empty());
			CHECK(summary.Axes == std::map<std::string, float>{ { "Action.MoveX", 1.0f }, { "Gamepad.LeftY", -0.5f } });
			// The frame view is latched separately: nothing yet.
			CHECK(input.GetSummary(InputPhase::Frame) == PlayInputSummary{});
			input.LatchFrame();
			CHECK(input.GetSummary(InputPhase::Frame).Down == summary.Down);
		}

		TEST_CASE("PlayInput: an injected action is down until released, and an injected value holds until changed")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const uint32_t jump = *input.GetActions().FindAction("Jump");
			const uint32_t moveX = *input.GetActions().FindAction("MoveX");
			REQUIRE(input.Queue(0, MakeActionEvent("Jump", PlayInputEventState::Down)).has_value());
			PlayInputEvent axis = MakeActionEvent("MoveX", PlayInputEventState::Down);
			axis.HasValue = true;
			axis.Value = -0.25f;
			REQUIRE(input.Queue(0, axis).has_value());
			input.ApplyTick(0);
			input.ApplyTick(1);
			CHECK(input.IsActionDown(InputPhase::Step, jump));
			CHECK_FALSE(input.WasActionPressed(InputPhase::Step, jump));
			CHECK(input.GetActionAxis(InputPhase::Step, jump) == 1.0f); // a Button action's axis is 1 while down
			CHECK(input.GetActionAxis(InputPhase::Step, moveX) == doctest::Approx(-0.25f));

			REQUIRE(input.Queue(2, MakeActionEvent("Jump", PlayInputEventState::Up)).has_value());
			input.ApplyTick(2);
			CHECK_FALSE(input.IsActionDown(InputPhase::Step, jump));
			CHECK(input.WasActionReleased(InputPhase::Step, jump));
			CHECK(input.GetActionAxis(InputPhase::Step, moveX) == doctest::Approx(-0.25f));
			// A tap on a value is invalid: a value event sets no state.
			PlayInputEvent tappedValue = axis;
			tappedValue.State = PlayInputEventState::Tap;
			CHECK_FALSE(input.Queue(3, tappedValue).has_value());
		}

		TEST_CASE("PlayInput: QueueReleaseAll releases the latest queued state before the tick's other events")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			PlayInputEvent key;
			key.Type = PlayInputEventType::KeyInput;
			key.KeyCode = Key::A;
			REQUIRE(input.Queue(0, key).has_value());
			REQUIRE(input.Queue(0, MakeActionEvent("Jump", PlayInputEventState::Down)).has_value());
			input.ApplyTick(0);
			// Queued but not applied yet: part of the latest queued state too.
			PlayInputEvent button;
			button.Type = PlayInputEventType::GamepadButtonInput;
			button.GamepadButtonCode = GamepadButton::East;
			REQUIRE(input.Queue(1, button).has_value());
			PlayInputEvent again = key;
			again.KeyCode = Key::B;
			REQUIRE(input.Queue(2, again).has_value());

			input.QueueReleaseAll(2);
			input.ApplyTick(1);
			CHECK(input.GetDevices().IsGamepadButtonDown(InputPhase::Step, 0, GamepadButton::East));
			input.ApplyTick(2);
			// The releases come first in tick 2, then its own press of B.
			const PlayInputSummary summary = input.GetSummary(InputPhase::Step);
			CHECK(summary.Down == std::vector<std::string>{ "Key.B" });
			CHECK(summary.Released == std::vector<std::string>{ "Action.Jump", "Action.MoveX", "Gamepad.East", "Key.A" });
			CHECK(input.GetLastAppliedEvents().back().KeyCode == Key::B);
		}

		TEST_CASE("PlayInput: device events are queued for the next tick, gamepad connections apply at once")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Repeated, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.QueueDeviceEvent(Event(CharEvent{ .Codepoint = 0x00E9, .Handled = false }));
			input.QueueDeviceEvent(Event(WindowFocusEvent{ .Focused = true, .Handled = false }));
			input.QueueDeviceEvent(Event(GamepadEvent{ .Gamepad = 1,
				.Kind = GamepadEventKind::Connected,
				.Button = GamepadButton::South,
				.Pressed = false,
				.Axis = GamepadAxis::LeftX,
				.Value = 0.0f,
				.Handled = false }));
			CHECK(input.GetQueuedEventCount() == 2); // the key press and the character; the repeat and the focus are no input
			input.ApplyTick(0);
			CHECK(input.GetDevices().IsGamepadConnected(InputPhase::Step, 1));
			CHECK(input.WasActionPressed(InputPhase::Step, *input.GetActions().FindAction("Jump")));
			REQUIRE(input.GetLastAppliedEvents().size() == 2);
			CHECK(input.GetLastAppliedEvents()[1].Text == "\xC3\xA9");
		}

		TEST_CASE("PlayInput: continuous device input is coalesced while its tick has not run")
		{
			// A session that does not advance (paused, or between the ticks of a slow FixedHz) keeps one event per continuous
			// source, not one per frame, and the tick's step view is the same as without coalescing.
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const auto gamepadAxis = [](uint32_t gamepad, GamepadAxis axis, float value)
			{
				return Event(GamepadEvent{ .Gamepad = gamepad,
					.Kind = GamepadEventKind::Axis,
					.Button = GamepadButton::South,
					.Pressed = false,
					.Axis = axis,
					.Value = value,
					.Handled = false });
			};
			input.QueueDeviceEvent(Event(MouseMoveEvent{ .Position = glm::vec2(1.0f, 2.0f), .Handled = false }));
			input.ApplyTick(0); // the cursor's first position, from which later moves count
			for (int index = 1; index <= 10000; ++index)
			{
				const float coordinate = static_cast<float>(index);
				input.QueueDeviceEvent(Event(MouseMoveEvent{ .Position = glm::vec2(coordinate, 2.0f * coordinate), .Handled = false }));
				input.QueueDeviceEvent(gamepadAxis(0, GamepadAxis::LeftX, static_cast<float>(index % 100) / 100.0f));
				input.QueueDeviceEvent(gamepadAxis(0, GamepadAxis::LeftY, -0.5f));
				input.QueueDeviceEvent(Event(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 1.0f), .Handled = false }));
			}
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.QueueDeviceEvent(gamepadAxis(0, GamepadAxis::LeftX, 0.75f));
			// The cursor, each gamepad axis and the scroll once each, and the key press.
			CHECK(input.GetQueuedEventCount() == 5);

			input.ApplyTick(1);
			const InputState& devices = input.GetDevices();
			CHECK(devices.GetMousePosition(InputPhase::Step) == glm::vec2(10000.0f, 20000.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Step) == glm::vec2(9999.0f, 19998.0f));
			CHECK(devices.GetScrollDelta(InputPhase::Step) == glm::vec2(0.0f, 10000.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Step, 0, GamepadAxis::LeftX) == 0.75f);
			CHECK(devices.GetGamepadAxis(InputPhase::Step, 0, GamepadAxis::LeftY) == -0.5f);
			CHECK(input.WasActionPressed(InputPhase::Step, *input.GetActions().FindAction("Jump")));
			CHECK(input.GetQueuedEventCount() == 0);
		}

		TEST_CASE("PlayInput: invalid project actions are reported below /Input/Actions")
		{
			InputSettings settings;
			InputActionSettings broken;
			broken.Bindings = { "Key.Nope" };
			settings.Actions.emplace("Broken", broken);
			const Result<PlayInput> created = PlayInput::Create(settings);
			REQUIRE_FALSE(created.has_value());
			CHECK(created.error().GetCode() == ErrorCode::Validation);
			REQUIRE_FALSE(created.error().GetIssues().empty());
			CHECK(created.error().GetIssues().front().JsonPointer.starts_with("/Input/Actions/Broken"));
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
