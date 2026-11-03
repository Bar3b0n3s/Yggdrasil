#include "TestsPCH.h"

#include "Engine/Session/PlayInput.h"

#include "Engine/Project/ProjectSettings.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// PlayInput (Architecture §4.3, §5.7 step 1, §13.6; Docs/Decisions/0012-m7-decisions.md decision 4): the tick-stamped
// queue, taps, injected actions combined with bindings, the views play.state reports, and the frame preview (M22 W-CLOCK,
// Docs/Decisions/0033-m22-contract.md decision 12).

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

		// A key event as the host's window reports one (QueueDeviceEvent).
		Event MakeKeyEvent(Key key, ButtonAction action)
		{
			return KeyEvent{ .KeyCode = key, .Action = action, .Modifiers = KeyModifiers::None, .Handled = false };
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

		// W-CLOCK (M22, C22.1; Docs/Decisions/0033-m22-contract.md decision 12; stream K): Roadmap M22's acceptance "a frame that
		// runs no step sees new input while the step view is unchanged".
		TEST_CASE("PlayInput: a frame that runs no step sees new device input while the step view is unchanged")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const uint32_t jump = *input.GetActions().FindAction("Jump");
			input.SetFramePreview(true);
			CHECK(input.IsFramePreviewEnabled());
			input.ApplyTick(0);
			input.LatchFrame();

			// Space goes down between two ticks: the next frame, which runs no step, sees it pressed.
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.LatchFrame();
			CHECK(input.GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			CHECK(input.GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK(input.WasActionPressed(InputPhase::Frame, jump));
			// The step view, the queue and the next tick are unchanged.
			CHECK_FALSE(input.GetDevices().IsKeyDown(InputPhase::Step, Key::Space));
			CHECK_FALSE(input.IsActionDown(InputPhase::Step, jump));
			CHECK(input.GetQueuedEventCount() == 1);
			CHECK(input.GetNextTick() == 1);

			// The tick applies the press as before: the step view sees it once.
			input.ApplyTick(1);
			CHECK(input.GetDevices().WasKeyPressed(InputPhase::Step, Key::Space));
			REQUIRE(input.GetLastAppliedEvents().size() == 1);
		}

		// M22 C22.3 (Docs/Decisions/0033-m22-contract.md decision 46; stream T): the text latch.
		TEST_CASE("PlayInput: the text latch holds the tick's committed characters in the step view and the frame's since the last frame"
			* doctest::skip(true))
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			input.SetFramePreview(true);
			CHECK(input.GetText(InputPhase::Step).empty());
			CHECK(input.GetText(InputPhase::Frame).empty());

			// Tick 0: an injected Text event, then a device character (both UTF-8, both committed in queue order).
			REQUIRE(input.Queue(0, PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = "h\xC3\xA9" }).has_value());
			input.QueueDeviceEvent(Event(CharEvent{ .Codepoint = 0x05D0, .Handled = false }));
			input.ApplyTick(0);
			input.LatchStepText();
			CHECK(input.GetText(InputPhase::Step) == "h\xC3\xA9\xD7\x90");

			// A device character typed before the next frame is never previewed: the frame sees tick 0's text only.
			input.QueueDeviceEvent(Event(CharEvent{ .Codepoint = 'x', .Handled = false }));
			input.LatchFrame();
			CHECK(input.GetText(InputPhase::Frame) == "h\xC3\xA9\xD7\x90");

			// Two ticks in one frame: each step reads its own; the frame reads both, in order. A tick without Text events
			// latches no text, and a frame that ran no step reads none.
			input.ApplyTick(1);
			input.LatchStepText();
			CHECK(input.GetText(InputPhase::Step) == "x");
			input.ApplyTick(2);
			input.LatchStepText();
			CHECK(input.GetText(InputPhase::Step).empty());
			input.LatchFrame();
			CHECK(input.GetText(InputPhase::Frame) == "x");
			input.LatchFrame();
			CHECK(input.GetText(InputPhase::Frame).empty());
		}

		TEST_CASE("PlayInput: a previewed press is reported once by the frame view")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			input.SetFramePreview(true);
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.LatchFrame();
			CHECK(input.GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
			// A second frame before the tick, and the frame after the tick that applies it, see it down but not pressed again.
			input.LatchFrame();
			CHECK(input.GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			CHECK_FALSE(input.GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
			input.ApplyTick(0);
			input.LatchFrame();
			CHECK(input.GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			CHECK_FALSE(input.GetDevices().WasKeyPressed(InputPhase::Frame, Key::Space));
		}

		TEST_CASE("PlayInput: injected events are never previewed, and without the preview the frame view follows the ticks")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const uint32_t jump = *input.GetActions().FindAction("Jump");
			CHECK_FALSE(input.IsFramePreviewEnabled()); // off at creation
			input.SetFramePreview(true);
			REQUIRE(input.IsFramePreviewEnabled());
			REQUIRE(input.Queue(0, MakeActionEvent("Jump", PlayInputEventState::Down)).has_value());
			input.LatchFrame();
			CHECK_FALSE(input.IsActionDown(InputPhase::Frame, jump));

			input.SetFramePreview(false);
			CHECK_FALSE(input.IsFramePreviewEnabled());
			input.QueueDeviceEvent(Event(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Pressed, .Modifiers = KeyModifiers::None, .Handled = false }));
			input.LatchFrame();
			CHECK_FALSE(input.GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			input.ApplyTick(0);
			input.LatchFrame();
			CHECK(input.GetDevices().IsKeyDown(InputPhase::Frame, Key::Space));
			CHECK(input.IsActionDown(InputPhase::Frame, jump));
		}

		// W-CLOCK (decision 12): the preview changes the frame view only. Two inputs take the same device and injected input over
		// the same frames and ticks, one with the preview: every tick applies the same events and latches the same step view,
		// cursor and scroll sums included, and once the ticks have applied everything the frame views hold the same levels.
		TEST_CASE("PlayInput: the frame preview leaves every tick's applied events and step view as they are without it")
		{
			Result<PlayInput> previewedInput = PlayInput::Create(MakeActions());
			Result<PlayInput> plainInput = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(previewedInput.has_value(), previewedInput.error().ToString());
			REQUIRE_MESSAGE(plainInput.has_value(), plainInput.error().ToString());
			PlayInput& previewed = *previewedInput;
			PlayInput& plain = *plainInput;
			previewed.SetFramePreview(true);

			const auto device = [&previewed, &plain](const Event& event)
			{
				previewed.QueueDeviceEvent(event);
				plain.QueueDeviceEvent(event);
			};
			const auto frame = [&previewed, &plain]()
			{
				previewed.LatchFrame();
				plain.LatchFrame();
			};
			const auto tick = [&previewed, &plain]()
			{
				REQUIRE(previewed.GetNextTick() == plain.GetNextTick());
				const uint64_t next = plain.GetNextTick();
				CAPTURE(next);
				previewed.ApplyTick(next);
				plain.ApplyTick(next);
				CHECK(std::ranges::equal(previewed.GetLastAppliedEvents(), plain.GetLastAppliedEvents()));
				CHECK(previewed.GetSummary(InputPhase::Step) == plain.GetSummary(InputPhase::Step));
				const InputState& withPreview = previewed.GetDevices();
				const InputState& without = plain.GetDevices();
				CHECK(withPreview.GetMousePosition(InputPhase::Step) == without.GetMousePosition(InputPhase::Step));
				CHECK(withPreview.GetMouseDelta(InputPhase::Step) == without.GetMouseDelta(InputPhase::Step));
				CHECK(withPreview.GetScrollDelta(InputPhase::Step) == without.GetScrollDelta(InputPhase::Step));
				for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
					CHECK(withPreview.IsGamepadConnected(InputPhase::Step, gamepad) == without.IsGamepadConnected(InputPhase::Step, gamepad));
			};

			// The cursor's first position, a key press, and a gamepad that connects at once with its stick queued.
			device(MouseMoveEvent{ .Position = glm::vec2(10.5f, 20.25f), .Handled = false });
			device(MakeKeyEvent(Key::A, ButtonAction::Pressed));
			device(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Connected });
			device(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Axis, .Axis = GamepadAxis::LeftX, .Value = 0.5f });
			frame();
			// The cursor moves again (coalesced), the wheel turns, A is released within the tick, a gamepad button goes down, and
			// automation queues D for the same tick, after the device input the frame already previewed.
			device(MouseMoveEvent{ .Position = glm::vec2(13.75f, 18.5f), .Handled = false });
			device(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 1.5f), .Handled = false });
			device(MakeKeyEvent(Key::A, ButtonAction::Released));
			device(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = true });
			const PlayInputEvent pressD{ .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Down, .KeyCode = Key::D };
			REQUIRE(previewed.Queue(previewed.GetNextTick(), pressD).has_value());
			REQUIRE(plain.Queue(plain.GetNextTick(), pressD).has_value());
			frame();
			tick();
			// Two wheel turns, summed in the queue, one of them previewed before the other arrives, and a cursor move.
			device(MouseScrollEvent{ .Offset = glm::vec2(0.25f, 0.0f), .Handled = false });
			frame();
			device(MouseScrollEvent{ .Offset = glm::vec2(0.0f, -0.75f), .Handled = false });
			device(MouseMoveEvent{ .Position = glm::vec2(14.125f, 18.5f), .Handled = false });
			frame();
			tick();
			// Space goes down and is previewed, then input.inject {releaseAll} puts its releases before that press.
			device(MakeKeyEvent(Key::Space, ButtonAction::Pressed));
			frame();
			previewed.QueueReleaseAll(previewed.GetNextTick());
			plain.QueueReleaseAll(plain.GetNextTick());
			device(MouseMoveEvent{ .Position = glm::vec2(1.0f, 2.0f), .Handled = false });
			frame();
			tick();
			// The stick moves and is previewed, then the gamepad disconnects at once, before the tick that applies the stick.
			device(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Axis, .Axis = GamepadAxis::LeftX, .Value = -0.25f });
			frame();
			device(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Disconnected });
			tick();
			frame();
			// Two ticks in one frame, the second empty.
			device(MakeKeyEvent(Key::Space, ButtonAction::Released));
			frame();
			tick();
			tick();
			frame();
			frame();
			CHECK(previewed.GetSummary(InputPhase::Frame) == plain.GetSummary(InputPhase::Frame));
			CHECK(previewed.GetDevices().GetMousePosition(InputPhase::Frame) == plain.GetDevices().GetMousePosition(InputPhase::Frame));
			CHECK(previewed.GetDevices().GetMouseDelta(InputPhase::Frame) == plain.GetDevices().GetMouseDelta(InputPhase::Frame));
		}

		// A session turns the preview off in lockstep and while recording (decision 12): from the first frame it latches without
		// the preview, its frame view is the applied ticks' one, as if it had never previewed, and a later preview starts anew.
		TEST_CASE("PlayInput: a frame latched without the preview is the applied ticks' frame view, even right after a preview")
		{
			Result<PlayInput> previewedInput = PlayInput::Create(MakeActions());
			Result<PlayInput> plainInput = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(previewedInput.has_value(), previewedInput.error().ToString());
			REQUIRE_MESSAGE(plainInput.has_value(), plainInput.error().ToString());
			PlayInput& previewed = *previewedInput;
			PlayInput& plain = *plainInput;
			const InputState& devices = previewed.GetDevices();
			previewed.SetFramePreview(true);
			previewed.QueueDeviceEvent(MakeKeyEvent(Key::Space, ButtonAction::Pressed));
			plain.QueueDeviceEvent(MakeKeyEvent(Key::Space, ButtonAction::Pressed));
			previewed.LatchFrame();
			plain.LatchFrame();
			REQUIRE(devices.WasKeyPressed(InputPhase::Frame, Key::Space));

			previewed.SetFramePreview(false);
			previewed.LatchFrame();
			plain.LatchFrame();
			CHECK_FALSE(devices.IsKeyDown(InputPhase::Frame, Key::Space));
			CHECK(previewed.GetSummary(InputPhase::Frame) == plain.GetSummary(InputPhase::Frame));
			previewed.ApplyTick(0);
			plain.ApplyTick(0);
			previewed.LatchFrame();
			plain.LatchFrame();
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK(previewed.GetSummary(InputPhase::Frame) == plain.GetSummary(InputPhase::Frame));

			// Device input previewed, then latched without the preview, is previewed again once the preview is back on.
			previewed.QueueDeviceEvent(MakeKeyEvent(Key::A, ButtonAction::Pressed));
			previewed.SetFramePreview(true);
			previewed.LatchFrame();
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::A));
			previewed.SetFramePreview(false);
			previewed.LatchFrame();
			CHECK_FALSE(devices.IsKeyDown(InputPhase::Frame, Key::A));
			previewed.SetFramePreview(true);
			previewed.LatchFrame();
			CHECK(devices.IsKeyDown(InputPhase::Frame, Key::A));
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::A));
		}

		TEST_CASE("PlayInput: the frame view previews cursor motion, scrolling and stick values, each part once")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const InputState& devices = input.GetDevices();
			input.SetFramePreview(true);
			input.QueueDeviceEvent(MouseMoveEvent{ .Position = glm::vec2(10.0f, 10.0f), .Handled = false });
			input.QueueDeviceEvent(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Connected });
			input.ApplyTick(0);
			input.LatchFrame();

			// Between two ticks the cursor moves twice (coalesced in the queue), the wheel turns twice (summed) and the stick moves:
			// each frame sees what arrived since the frame before.
			input.QueueDeviceEvent(MouseMoveEvent{ .Position = glm::vec2(14.0f, 12.0f), .Handled = false });
			input.QueueDeviceEvent(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 1.0f), .Handled = false });
			input.QueueDeviceEvent(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Axis, .Axis = GamepadAxis::LeftY, .Value = 0.5f });
			input.LatchFrame();
			CHECK(devices.GetMousePosition(InputPhase::Frame) == glm::vec2(14.0f, 12.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Frame) == glm::vec2(4.0f, 2.0f));
			CHECK(devices.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f, 1.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Frame, 1, GamepadAxis::LeftY) == 0.5f);
			input.QueueDeviceEvent(MouseMoveEvent{ .Position = glm::vec2(20.0f, 12.0f), .Handled = false });
			input.QueueDeviceEvent(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 2.0f), .Handled = false });
			input.LatchFrame();
			CHECK(devices.GetMousePosition(InputPhase::Frame) == glm::vec2(20.0f, 12.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Frame) == glm::vec2(6.0f, 0.0f));
			CHECK(devices.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f, 2.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Frame, 1, GamepadAxis::LeftY) == 0.5f);
			// The step view still holds the last tick's state.
			CHECK(devices.GetMousePosition(InputPhase::Step) == glm::vec2(10.0f, 10.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Step, 1, GamepadAxis::LeftY) == 0.0f);

			// The tick applies the coalesced input as one step; the frame after it has nothing new to report.
			input.ApplyTick(1);
			CHECK(devices.GetMousePosition(InputPhase::Step) == glm::vec2(20.0f, 12.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Step) == glm::vec2(10.0f, 2.0f));
			CHECK(devices.GetScrollDelta(InputPhase::Step) == glm::vec2(0.0f, 3.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Step, 1, GamepadAxis::LeftY) == 0.5f);
			input.LatchFrame();
			CHECK(devices.GetMousePosition(InputPhase::Frame) == glm::vec2(20.0f, 12.0f));
			CHECK(devices.GetMouseDelta(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK(devices.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK(devices.GetGamepadAxis(InputPhase::Frame, 1, GamepadAxis::LeftY) == 0.5f);
		}

		TEST_CASE("PlayInput: a device tap between two frames, and a release after the preview, reach the frame view once")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const InputState& devices = input.GetDevices();
			input.SetFramePreview(true);

			// Space is pressed and released before the next frame: the frame reports both edges, as the step will.
			input.QueueDeviceEvent(MakeKeyEvent(Key::Space, ButtonAction::Pressed));
			input.QueueDeviceEvent(MakeKeyEvent(Key::Space, ButtonAction::Released));
			input.LatchFrame();
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK(devices.WasKeyReleased(InputPhase::Frame, Key::Space));
			CHECK_FALSE(devices.IsKeyDown(InputPhase::Frame, Key::Space));
			input.ApplyTick(0);
			CHECK(devices.WasKeyPressed(InputPhase::Step, Key::Space));
			CHECK(devices.WasKeyReleased(InputPhase::Step, Key::Space));
			input.LatchFrame();
			CHECK_FALSE(devices.WasKeyPressed(InputPhase::Frame, Key::Space));
			CHECK_FALSE(devices.WasKeyReleased(InputPhase::Frame, Key::Space));

			// A press the frame previewed and a release that arrives after that frame, before the tick: the frame after the tick
			// reports only the release.
			input.QueueDeviceEvent(MakeKeyEvent(Key::A, ButtonAction::Pressed));
			input.LatchFrame();
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::A));
			input.QueueDeviceEvent(MakeKeyEvent(Key::A, ButtonAction::Released));
			input.ApplyTick(1);
			input.LatchFrame();
			CHECK_FALSE(devices.WasKeyPressed(InputPhase::Frame, Key::A));
			CHECK(devices.WasKeyReleased(InputPhase::Frame, Key::A));
			CHECK_FALSE(devices.IsKeyDown(InputPhase::Frame, Key::A));
		}

		// Injected input queued for a tick before a device event the frame previewed reaches the frame view through the levels
		// the tick leaves: the frame view saw the device event first, so it never reports an edge the ticks did not make.
		TEST_CASE("PlayInput: after a tick the frame view holds the applied levels, whatever the tick queued before a previewed event")
		{
			Result<PlayInput> created = PlayInput::Create(MakeActions());
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PlayInput& input = *created;
			const InputState& devices = input.GetDevices();
			input.SetFramePreview(true);
			input.QueueDeviceEvent(MakeKeyEvent(Key::A, ButtonAction::Pressed));
			input.ApplyTick(0);
			input.LatchFrame();
			input.QueueDeviceEvent(MakeKeyEvent(Key::B, ButtonAction::Pressed));
			input.LatchFrame();
			CHECK(devices.WasKeyPressed(InputPhase::Frame, Key::B));
			CHECK(devices.IsKeyDown(InputPhase::Frame, Key::A));

			// input.inject {releaseAll} for the next tick: its releases go before the queued press of B, so the tick releases A and
			// leaves B down.
			input.QueueReleaseAll(input.GetNextTick());
			input.ApplyTick(1);
			CHECK(devices.WasKeyReleased(InputPhase::Step, Key::A));
			CHECK(devices.WasKeyPressed(InputPhase::Step, Key::B));
			CHECK(devices.IsKeyDown(InputPhase::Step, Key::B));
			input.LatchFrame();
			CHECK(devices.WasKeyReleased(InputPhase::Frame, Key::A));
			CHECK_FALSE(devices.IsKeyDown(InputPhase::Frame, Key::A));
			CHECK(devices.IsKeyDown(InputPhase::Frame, Key::B));
			CHECK_FALSE(devices.WasKeyPressed(InputPhase::Frame, Key::B));
			CHECK_FALSE(devices.WasKeyReleased(InputPhase::Frame, Key::B));
			CHECK(input.GetSummary(InputPhase::Frame).Down == input.GetSummary(InputPhase::Step).Down);
		}
	}

}
