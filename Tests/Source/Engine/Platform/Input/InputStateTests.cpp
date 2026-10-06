#include "TestsPCH.h"

#include "Engine/Platform/Input/InputState.h"

#include "Engine/Platform/Input/InputActionMap.h"

#include <cmath>
#include <limits>

namespace Engine {

	static Event KeyDown(Key key)
	{
		return KeyEvent{ .KeyCode = key, .Action = ButtonAction::Pressed };
	}

	static Event KeyUp(Key key)
	{
		return KeyEvent{ .KeyCode = key, .Action = ButtonAction::Released };
	}

	static Event GamepadConnected(uint32_t gamepad)
	{
		return GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Connected };
	}

	static Event GamepadAxisMoved(uint32_t gamepad, GamepadAxis axis, float value)
	{
		return GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Axis, .Axis = axis, .Value = value };
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("InputState: a tap between two steps reports Pressed and Released on the next step")
		{
			InputState input;
			input.LatchStep();

			input.Inject(KeyDown(Key::Space));
			input.Inject(KeyUp(Key::Space));
			input.Inject(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Pressed });
			input.Inject(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Released });
			input.Inject(GamepadConnected(0));
			input.Inject(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = true });
			input.Inject(GamepadEvent{ .Gamepad = 0, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = false });
			input.LatchStep();

			CHECK(input.WasKeyPressed(InputPhase::Step, Key::Space));
			CHECK(input.WasKeyReleased(InputPhase::Step, Key::Space));
			CHECK_FALSE(input.IsKeyDown(InputPhase::Step, Key::Space));
			CHECK(input.WasMouseButtonPressed(InputPhase::Step, MouseButton::Left));
			CHECK(input.WasMouseButtonReleased(InputPhase::Step, MouseButton::Left));
			CHECK_FALSE(input.IsMouseButtonDown(InputPhase::Step, MouseButton::Left));
			CHECK(input.WasGamepadButtonPressed(InputPhase::Step, 0, GamepadButton::South));
			CHECK(input.WasGamepadButtonReleased(InputPhase::Step, 0, GamepadButton::South));
			CHECK_FALSE(input.IsGamepadButtonDown(InputPhase::Step, 0, GamepadButton::South));

			// The step after reports nothing: each edge is seen exactly once.
			input.LatchStep();
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, Key::Space));
			CHECK_FALSE(input.WasKeyReleased(InputPhase::Step, Key::Space));
			CHECK_FALSE(input.WasMouseButtonPressed(InputPhase::Step, MouseButton::Left));
			CHECK_FALSE(input.WasGamepadButtonReleased(InputPhase::Step, 0, GamepadButton::South));
		}

		TEST_CASE("InputState: zero-step frames keep step edges pending")
		{
			InputState input;

			// Frame 1 runs no step: the frame view sees the press, the step view does not yet.
			input.Inject(KeyDown(Key::W));
			input.LatchFrame();
			CHECK(input.WasKeyPressed(InputPhase::Frame, Key::W));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, Key::W));
			CHECK_FALSE(input.IsKeyDown(InputPhase::Step, Key::W));

			// Frame 2 runs no step either, and the key is released.
			input.Inject(KeyUp(Key::W));
			input.LatchFrame();
			CHECK(input.WasKeyReleased(InputPhase::Frame, Key::W));
			CHECK_FALSE(input.WasKeyReleased(InputPhase::Step, Key::W));

			// Frame 3 runs one step: both pending edges arrive together.
			input.LatchFrame();
			input.LatchStep();
			CHECK(input.WasKeyPressed(InputPhase::Step, Key::W));
			CHECK(input.WasKeyReleased(InputPhase::Step, Key::W));
			CHECK_FALSE(input.IsKeyDown(InputPhase::Step, Key::W));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Frame, Key::W));
		}

		TEST_CASE("InputState: multi-step frames report an edge once")
		{
			InputState input;
			input.Inject(KeyDown(Key::A));
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 3.0f) });

			// One frame with three steps.
			input.LatchStep();
			CHECK(input.WasKeyPressed(InputPhase::Step, Key::A));
			CHECK(input.IsKeyDown(InputPhase::Step, Key::A));
			CHECK(input.GetScrollDelta(InputPhase::Step) == glm::vec2(0.0f, 3.0f));
			for (int step = 1; step < 3; ++step)
			{
				input.LatchStep();
				CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, Key::A));
				CHECK(input.IsKeyDown(InputPhase::Step, Key::A));
				CHECK(input.GetScrollDelta(InputPhase::Step) == glm::vec2(0.0f));
			}
			input.LatchFrame();
			CHECK(input.WasKeyPressed(InputPhase::Frame, Key::A));
			CHECK(input.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f, 3.0f));
		}

		TEST_CASE("InputState: frame edges are independent of step edges")
		{
			// The steps run per frame (0, 1, 2 and 5, as the Timing suite does) never change what the frame view reports.
			const std::array<int, 4> stepsPerFrame = { 0, 1, 2, 5 };
			for (const int steps : stepsPerFrame)
			{
				CAPTURE(steps);
				InputState input;
				input.Inject(KeyDown(Key::Enter));
				for (int step = 0; step < steps; ++step)
					input.LatchStep();
				input.LatchFrame();
				CHECK(input.WasKeyPressed(InputPhase::Frame, Key::Enter));
				CHECK(input.IsKeyDown(InputPhase::Frame, Key::Enter));

				input.Inject(KeyUp(Key::Enter));
				for (int step = 0; step < steps; ++step)
					input.LatchStep();
				input.LatchFrame();
				CHECK(input.WasKeyReleased(InputPhase::Frame, Key::Enter));
				CHECK_FALSE(input.WasKeyPressed(InputPhase::Frame, Key::Enter));
				CHECK_FALSE(input.IsKeyDown(InputPhase::Frame, Key::Enter));

				// The step view reported each edge on the first step after it, or still holds it when no step ran.
				input.LatchStep();
				CHECK(input.WasKeyReleased(InputPhase::Step, Key::Enter) == (steps == 0));
				CHECK(input.WasKeyPressed(InputPhase::Step, Key::Enter) == (steps == 0));
			}
		}

		TEST_CASE("InputState: stick Y is up-positive and Invert flips it")
		{
			// GLFW reports a stick pushed up as -1 on its Y axis.
			struct Row
			{
				GamepadAxis Axis = GamepadAxis::LeftX;
				float GlfwValue = 0.0f;
				bool Invert = false;
				float ExpectedState = 0.0f;  // InputState::GetGamepadAxis
				float ExpectedAction = 0.0f; // InputActionMap::GetAxis with the dead zone
			};
			const std::array<Row, 14> rows = { {
				{ GamepadAxis::LeftY, -1.0f, false, 1.0f, 1.0f },
				{ GamepadAxis::LeftY, 1.0f, false, -1.0f, -1.0f },
				{ GamepadAxis::LeftY, -1.0f, true, 1.0f, -1.0f },
				{ GamepadAxis::LeftY, 1.0f, true, -1.0f, 1.0f },
				{ GamepadAxis::RightY, -1.0f, false, 1.0f, 1.0f },
				{ GamepadAxis::RightY, -1.0f, true, 1.0f, -1.0f },
				{ GamepadAxis::LeftX, 1.0f, false, 1.0f, 1.0f },
				{ GamepadAxis::RightX, -1.0f, true, -1.0f, 1.0f },
				{ GamepadAxis::LeftY, -0.1f, false, 0.1f, 0.0f },
				{ GamepadAxis::LeftY, 0.0f, true, 0.0f, 0.0f },
				// Triggers: GLFW's -1 (released) to 1 becomes 0 to 1, so a released trigger reads 0 like a centred stick.
				{ GamepadAxis::LeftTrigger, -1.0f, false, 0.0f, 0.0f },
				{ GamepadAxis::RightTrigger, 1.0f, false, 1.0f, 1.0f },
				{ GamepadAxis::RightTrigger, 0.0f, false, 0.5f, (0.5f - 0.15f) / 0.85f },
				{ GamepadAxis::LeftTrigger, -0.8f, false, 0.1f, 0.0f },
			} };

			for (const Row& row : rows)
			{
				const std::string axisName(GamepadAxisToString(row.Axis));
				CAPTURE(axisName);
				CAPTURE(row.GlfwValue);
				CAPTURE(row.Invert);

				const float converted = GamepadAxisValueFromGlfw(row.Axis, row.GlfwValue);
				CHECK(converted == doctest::Approx(row.ExpectedState));

				InputState input;
				input.Inject(GamepadConnected(0));
				input.Inject(GamepadAxisMoved(0, row.Axis, converted));
				input.LatchStep();
				CHECK(input.GetGamepadAxis(InputPhase::Step, 0, row.Axis) == doctest::Approx(row.ExpectedState));

				const std::array<InputActionDefinition, 1> definitions = { {
					{ .Name = "Move", .Type = InputActionType::Axis, .Gamepad = "Gamepad." + axisName, .Invert = row.Invert },
				} };
				const Result<InputActionMap> actions = InputActionMap::Create(definitions);
				REQUIRE(actions.has_value());
				const std::optional<uint32_t> move = actions->FindAction("Move");
				REQUIRE(move.has_value());
				CHECK(actions->GetAxis(input, InputPhase::Step, *move) == doctest::Approx(row.ExpectedAction));
			}

			// Out-of-range device values are clamped: sticks to [-1, 1], triggers to [0, 1].
			CHECK(GamepadAxisValueFromGlfw(GamepadAxis::LeftX, 1.5f) == 1.0f);
			CHECK(GamepadAxisValueFromGlfw(GamepadAxis::LeftY, 2.0f) == -1.0f);
			CHECK(GamepadAxisValueFromGlfw(GamepadAxis::LeftTrigger, -1.5f) == 0.0f);
			CHECK(GamepadAxisValueFromGlfw(GamepadAxis::RightTrigger, 1.5f) == 1.0f);
		}

		TEST_CASE("InputState: the cursor delta and scroll accumulate per phase")
		{
			InputState input;
			// The first move sets the position without a delta.
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(100.0f, 50.0f) });
			input.LatchStep();
			input.LatchFrame();
			CHECK(input.GetMousePosition(InputPhase::Frame) == glm::vec2(100.0f, 50.0f));
			CHECK(input.GetMouseDelta(InputPhase::Frame) == glm::vec2(0.0f));

			input.Inject(MouseMoveEvent{ .Position = glm::vec2(110.0f, 40.0f) });
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(130.0f, 45.0f) });
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 1.0f) });
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(0.5f, -3.0f) });
			input.LatchStep();
			CHECK(input.GetMouseDelta(InputPhase::Step) == glm::vec2(30.0f, -5.0f));
			CHECK(input.GetScrollDelta(InputPhase::Step) == glm::vec2(0.5f, -2.0f));
			input.LatchStep();
			CHECK(input.GetMouseDelta(InputPhase::Step) == glm::vec2(0.0f));
			CHECK(input.GetMousePosition(InputPhase::Step) == glm::vec2(130.0f, 45.0f));

			input.LatchFrame();
			CHECK(input.GetMouseDelta(InputPhase::Frame) == glm::vec2(30.0f, -5.0f));
			CHECK(input.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.5f, -2.0f));
		}

		TEST_CASE("InputState: a disconnected gamepad releases its buttons and zeroes its axes")
		{
			InputState input;
			input.Inject(GamepadConnected(1));
			input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Button, .Button = GamepadButton::North, .Pressed = true });
			input.Inject(GamepadAxisMoved(1, GamepadAxis::RightTrigger, 1.0f));
			input.LatchFrame();
			CHECK(input.IsGamepadConnected(InputPhase::Frame, 1));
			CHECK_FALSE(input.IsGamepadConnected(InputPhase::Frame, 0));
			CHECK(input.IsGamepadButtonDown(InputPhase::Frame, 1, GamepadButton::North));
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 1, GamepadAxis::RightTrigger) == 1.0f);

			input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Disconnected });
			input.LatchFrame();
			CHECK_FALSE(input.IsGamepadConnected(InputPhase::Frame, 1));
			CHECK_FALSE(input.IsGamepadButtonDown(InputPhase::Frame, 1, GamepadButton::North));
			CHECK(input.WasGamepadButtonReleased(InputPhase::Frame, 1, GamepadButton::North));
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 1, GamepadAxis::RightTrigger) == 0.0f); // released
		}

		TEST_CASE("InputState: out-of-range codes are ignored and read as up")
		{
			InputState input;
			input.Inject(KeyDown(Key::None));
			input.Inject(KeyDown(static_cast<Key>(KeyCodeCount)));
			input.Inject(GamepadConnected(MaxGamepads));
			input.Inject(KeyEvent{ .KeyCode = Key::B, .Action = ButtonAction::Repeated });
			input.LatchStep();

			CHECK_FALSE(input.IsKeyDown(InputPhase::Step, Key::None));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, static_cast<Key>(KeyCodeCount)));
			CHECK_FALSE(input.IsGamepadConnected(InputPhase::Step, MaxGamepads));
			CHECK(input.GetGamepadAxis(InputPhase::Step, MaxGamepads, GamepadAxis::LeftX) == 0.0f);
			// A repeat without a press changes nothing.
			CHECK_FALSE(input.IsKeyDown(InputPhase::Step, Key::B));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, Key::B));
		}

		TEST_CASE("InputState: window, text and file-drop events leave the input state unchanged")
		{
			InputState input;
			input.Inject(WindowFocusEvent{ .Focused = false });
			input.Inject(WindowResizeEvent{ .Width = 10, .Height = 10, .FramebufferWidth = 20, .FramebufferHeight = 20 });
			input.Inject(CharEvent{ .Codepoint = 'x' });
			input.Inject(FileDropEvent{ .Paths = { "/tmp/a.png" } });
			input.Inject(WindowCloseEvent{});
			input.LatchFrame();

			CHECK(input.GetMousePosition(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK(input.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Frame, Key::X));
		}

		TEST_CASE("InputState: a press of a held key and a release of a released key are not edges")
		{
			InputState input;
			input.Inject(KeyDown(Key::G));
			input.LatchStep();
			REQUIRE(input.WasKeyPressed(InputPhase::Step, Key::G));

			input.Inject(KeyDown(Key::G));
			input.Inject(KeyUp(Key::H));
			input.Inject(MouseButtonEvent{ .Button = MouseButton::Middle, .Action = ButtonAction::Released });
			input.LatchStep();
			CHECK(input.IsKeyDown(InputPhase::Step, Key::G));
			CHECK_FALSE(input.WasKeyPressed(InputPhase::Step, Key::G));
			CHECK_FALSE(input.WasKeyReleased(InputPhase::Step, Key::H));
			CHECK_FALSE(input.WasMouseButtonReleased(InputPhase::Step, MouseButton::Middle));
		}

		TEST_CASE("InputState: a gamepad ignores button and axis events until it is connected")
		{
			InputState input;
			input.Inject(GamepadEvent{ .Gamepad = 2, .Kind = GamepadEventKind::Button, .Button = GamepadButton::East, .Pressed = true });
			input.Inject(GamepadAxisMoved(2, GamepadAxis::LeftX, 0.5f));
			input.LatchFrame();
			CHECK_FALSE(input.IsGamepadConnected(InputPhase::Frame, 2));
			CHECK_FALSE(input.IsGamepadButtonDown(InputPhase::Frame, 2, GamepadButton::East));
			CHECK_FALSE(input.WasGamepadButtonPressed(InputPhase::Frame, 2, GamepadButton::East));
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 2, GamepadAxis::LeftX) == 0.0f);

			input.Inject(GamepadConnected(2));
			input.Inject(GamepadEvent{ .Gamepad = 2, .Kind = GamepadEventKind::Button, .Button = GamepadButton::East, .Pressed = true });
			input.Inject(GamepadAxisMoved(2, GamepadAxis::LeftX, 0.5f));
			input.LatchFrame();
			CHECK(input.IsGamepadConnected(InputPhase::Frame, 2));
			CHECK(input.WasGamepadButtonPressed(InputPhase::Frame, 2, GamepadButton::East));
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 2, GamepadAxis::LeftX) == 0.5f);
		}

		TEST_CASE("InputState: axis values are clamped to their range and non-finite values are ignored")
		{
			constexpr float NotANumber = std::numeric_limits<float>::quiet_NaN();
			constexpr float Infinity = std::numeric_limits<float>::infinity();

			InputState input;
			input.Inject(GamepadConnected(0));
			input.Inject(GamepadAxisMoved(0, GamepadAxis::LeftX, 1.5f));
			input.Inject(GamepadAxisMoved(0, GamepadAxis::RightY, -3.0f));
			input.Inject(GamepadAxisMoved(0, GamepadAxis::LeftTrigger, -0.5f));
			input.Inject(GamepadAxisMoved(0, GamepadAxis::RightTrigger, 0.25f));
			input.Inject(GamepadAxisMoved(0, GamepadAxis::RightTrigger, NotANumber));
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(5.0f, 6.0f) });
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(Infinity, 6.0f) });
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(0.0f, NotANumber) });
			input.LatchFrame();

			CHECK(input.GetGamepadAxis(InputPhase::Frame, 0, GamepadAxis::LeftX) == 1.0f);
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 0, GamepadAxis::RightY) == -1.0f);
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 0, GamepadAxis::LeftTrigger) == 0.0f);
			CHECK(input.GetGamepadAxis(InputPhase::Frame, 0, GamepadAxis::RightTrigger) == 0.25f);
			CHECK(input.GetMousePosition(InputPhase::Frame) == glm::vec2(5.0f, 6.0f));
			CHECK(input.GetMouseDelta(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK(input.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f));
			CHECK(GamepadAxisValueFromGlfw(GamepadAxis::LeftX, NotANumber) == 0.0f);
		}

		TEST_CASE("InputState: finite input whose deltas would overflow a float never puts infinity or NaN in the views")
		{
			constexpr float Huge = 3e38f;
			InputState input;
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(Huge, 0.0f) });  // the first position: no delta
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(-Huge, 0.0f) }); // a delta of -6e38: ignored
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(Huge, 1.0f) });  // a delta of (0, 1) from where the cursor is
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(Huge, 0.0f) });
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(Huge, 0.0f) }); // the sum would be 6e38: ignored
			input.LatchStep();
			input.LatchFrame();

			for (const InputPhase phase : { InputPhase::Step, InputPhase::Frame })
			{
				const glm::vec2 delta = input.GetMouseDelta(phase);
				const glm::vec2 scroll = input.GetScrollDelta(phase);
				CHECK(std::isfinite(delta.x));
				CHECK(std::isfinite(delta.y));
				CHECK(std::isfinite(scroll.x));
				CHECK(std::isfinite(scroll.y));
				CHECK(input.GetMousePosition(phase) == glm::vec2(Huge, 1.0f));
				CHECK(delta == glm::vec2(0.0f, 1.0f));
				CHECK(scroll == glm::vec2(Huge, 0.0f));
			}

			// Ordinary motion after the ignored events still adds up.
			input.Inject(MouseMoveEvent{ .Position = glm::vec2(Huge, 3.0f) });
			input.Inject(MouseScrollEvent{ .Offset = glm::vec2(0.0f, 1.0f) });
			input.LatchFrame();
			CHECK(input.GetMouseDelta(InputPhase::Frame) == glm::vec2(0.0f, 2.0f));
			CHECK(input.GetScrollDelta(InputPhase::Frame) == glm::vec2(0.0f, 1.0f));
		}
	}

}
