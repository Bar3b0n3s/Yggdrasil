#include "TestsPCH.h"

#include "Engine/Platform/Input/InputActionMap.h"

#include "Support/DeathTest.h"

namespace Engine {

	static void Press(InputState& input, Key key)
	{
		input.Inject(KeyEvent{ .KeyCode = key, .Action = ButtonAction::Pressed });
	}

	static void Release(InputState& input, Key key)
	{
		input.Inject(KeyEvent{ .KeyCode = key, .Action = ButtonAction::Released });
	}

	static void MoveStick(InputState& input, uint32_t gamepad, GamepadAxis axis, float value)
	{
		input.Inject(GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Connected });
		input.Inject(GamepadEvent{ .Gamepad = gamepad, .Kind = GamepadEventKind::Axis, .Axis = axis, .Value = value });
	}

	// The project's MoveX action (Architecture §6.1) and a Jump button.
	static std::vector<InputActionDefinition> MakeDefinitions()
	{
		return {
			{
				.Name = "MoveX",
				.Type = InputActionType::Axis,
				.Positive = { "Key.D", "Key.Right" },
				.Negative = { "Key.A", "Key.Left" },
				.Gamepad = "Gamepad.LeftX",
			},
			{
				.Name = "Jump",
				.Type = InputActionType::Button,
				.Bindings = { "Key.Space", "Gamepad.South", "Mouse.Left" },
			},
		};
	}

	ENGINE_DEATH_TEST("Platform/InputActionIndexOutOfRange")
	{
		const std::vector<InputActionDefinition> definitions = MakeDefinitions();
		const Result<InputActionMap> actions = InputActionMap::Create(definitions);
		if (actions.has_value())
			static_cast<void>(actions->IsDown(InputState(), InputPhase::Step, 2));
	}

	TEST_SUITE("Platform")
	{
		TEST_CASE("InputActionMap: axis combines keys and gamepad with dead zone")
		{
			struct Row
			{
				std::vector<Key> KeysDown;
				std::optional<float> LeftX; // gamepad 0, engine convention; nullopt: no gamepad connected
				float Expected = 0.0f;
			};
			const std::vector<Row> rows = {
				{ {}, std::nullopt, 0.0f },
				{ { Key::D }, std::nullopt, 1.0f },
				{ { Key::Left }, std::nullopt, -1.0f },
				{ { Key::D, Key::A }, std::nullopt, 0.0f },
				{ { Key::Right, Key::D }, std::nullopt, 1.0f },
				{ {}, 0.1f, 0.0f },                  // inside the dead zone
				{ {}, 0.15f, 0.0f },                 // its edge
				{ {}, 0.575f, 0.5f },                // (0.575 - 0.15) / 0.85
				{ {}, -1.0f, -1.0f },                // full deflection stays full
				{ {}, 0.32f, 0.2f },                 // (0.32 - 0.15) / 0.85
				{ { Key::D }, -0.575f, 1.0f },       // the larger magnitude wins
				{ { Key::A }, 0.32f, -1.0f },        // the keys win against a smaller stick value
				{ { Key::D, Key::A }, 0.32f, 0.2f }, // cancelled keys leave the stick
				{ { Key::A }, 1.0f, -1.0f },         // a tie keeps the key value
			};

			const std::vector<InputActionDefinition> definitions = MakeDefinitions();
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE(actions.has_value());
			const std::optional<uint32_t> moveX = actions->FindAction("MoveX");
			REQUIRE(moveX.has_value());

			for (size_t index = 0; index < rows.size(); ++index)
			{
				const Row& row = rows[index];
				CAPTURE(index);
				InputState input;
				for (const Key key : row.KeysDown)
					Press(input, key);
				if (row.LeftX.has_value())
					MoveStick(input, 0, GamepadAxis::LeftX, *row.LeftX);
				input.LatchFrame();
				CHECK(actions->GetAxis(input, InputPhase::Frame, *moveX) == doctest::Approx(row.Expected));
			}

			// The stick of any connected gamepad counts; the largest magnitude wins.
			InputState twoPads;
			MoveStick(twoPads, 0, GamepadAxis::LeftX, 0.32f);
			MoveStick(twoPads, 2, GamepadAxis::LeftX, -0.575f);
			twoPads.LatchFrame();
			CHECK(actions->GetAxis(twoPads, InputPhase::Frame, *moveX) == doctest::Approx(-0.5f));

			CHECK(InputActionMap::ApplyDeadZone(0.0f) == 0.0f);
			CHECK(InputActionMap::ApplyDeadZone(-0.15f) == 0.0f);
			CHECK(InputActionMap::ApplyDeadZone(1.0f) == 1.0f);
			CHECK(InputActionMap::ApplyDeadZone(-1.0f) == -1.0f);
			CHECK(InputActionMap::ApplyDeadZone(2.0f) == 1.0f);
			CHECK(InputActionMap::ApplyDeadZone(0.575f) == doctest::Approx(0.5f));
		}

		TEST_CASE("InputActionMap: a button action is down while any binding is down and reports their edges")
		{
			const std::vector<InputActionDefinition> definitions = MakeDefinitions();
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE(actions.has_value());
			const std::optional<uint32_t> jump = actions->FindAction("Jump");
			REQUIRE(jump.has_value());

			InputState input;
			input.Inject(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Pressed });
			input.LatchStep();
			CHECK(actions->IsDown(input, InputPhase::Step, *jump));
			CHECK(actions->WasPressed(input, InputPhase::Step, *jump));
			CHECK(actions->GetAxis(input, InputPhase::Step, *jump) == 1.0f);

			// A tap of a second binding while the first is held: the union of the bindings' edges.
			Press(input, Key::Space);
			Release(input, Key::Space);
			input.LatchStep();
			CHECK(actions->IsDown(input, InputPhase::Step, *jump));
			CHECK(actions->WasPressed(input, InputPhase::Step, *jump));
			CHECK(actions->WasReleased(input, InputPhase::Step, *jump));

			input.Inject(MouseButtonEvent{ .Button = MouseButton::Left, .Action = ButtonAction::Released });
			input.LatchStep();
			CHECK_FALSE(actions->IsDown(input, InputPhase::Step, *jump));
			CHECK(actions->WasReleased(input, InputPhase::Step, *jump));
			CHECK(actions->GetAxis(input, InputPhase::Step, *jump) == 0.0f);

			// Button queries on an axis action read its Positive and Negative bindings.
			const std::optional<uint32_t> moveX = actions->FindAction("MoveX");
			REQUIRE(moveX.has_value());
			Press(input, Key::Left);
			input.LatchStep();
			CHECK(actions->IsDown(input, InputPhase::Step, *moveX));
			CHECK(actions->WasPressed(input, InputPhase::Step, *moveX));
		}

		TEST_CASE("InputActionMap: actions are indexed by name and found case-sensitively")
		{
			const std::vector<InputActionDefinition> definitions = MakeDefinitions();
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE(actions.has_value());
			REQUIRE(actions->GetActionCount() == 2);
			// "Jump" sorts before "MoveX" whatever the definition order.
			CHECK(actions->GetDefinition(0).Name == "Jump");
			CHECK(actions->GetDefinition(1).Name == "MoveX");
			CHECK(actions->FindAction("Jump") == 0u);
			CHECK(actions->FindAction("MoveX") == 1u);
			CHECK_FALSE(actions->FindAction("jump").has_value());
			CHECK_FALSE(actions->FindAction("Crouch").has_value());

			const InputActionMap empty;
			CHECK(empty.GetActionCount() == 0);
			CHECK_FALSE(empty.FindAction("Jump").has_value());
		}

		TEST_CASE("InputActionMap: invalid definitions are reported together with their pointers")
		{
			const std::vector<InputActionDefinition> definitions = {
				{ .Name = "Jump", .Type = InputActionType::Button, .Bindings = { "Key.Spacebar" } },
				{ .Name = "Jump", .Type = InputActionType::Button, .Bindings = { "Key.Space" } },
				{ .Name = "Fire", .Type = InputActionType::Button, .Bindings = { "Gamepad.LeftX" } },
				{ .Name = "Move", .Type = InputActionType::Axis, .Gamepad = "Gamepad.South" },
				{ .Name = "Look", .Type = InputActionType::Axis, .Bindings = { "Key.L" } },
				{ .Name = "Use", .Type = InputActionType::Button, .Bindings = { "Key.E" }, .Invert = true },
				{ .Name = "", .Type = InputActionType::Button },
			};
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE_FALSE(actions.has_value());
			CHECK(actions.error().GetCode() == ErrorCode::Validation);

			std::vector<std::string> pointers;
			for (const ErrorIssue& issue : actions.error().GetIssues())
				pointers.push_back(issue.JsonPointer);
			const auto contains = [&pointers](std::string_view pointer)
			{
				return std::ranges::find(pointers, pointer) != pointers.end();
			};
			CHECK(contains("/Jump/Bindings/0"));
			CHECK(contains("/Fire/Bindings/0"));
			CHECK(contains("/Move/Gamepad"));
			CHECK(contains("/Look/Bindings"));
			CHECK(contains("/Use/Invert"));
			CHECK(pointers.size() >= 7); // also the duplicated "Jump" and the empty name
		}

		TEST_CASE("InputBinding: names parse case-insensitively and print canonically")
		{
			const std::array<std::pair<std::string_view, std::string_view>, 6> names = { {
				{ "Key.Space", "Key.Space" },
				{ "key.space", "Key.Space" },
				{ "MOUSE.LEFT", "Mouse.Left" },
				{ "Gamepad.South", "Gamepad.South" },
				{ "gamepad.lefty", "Gamepad.LeftY" },
				{ "Key.Keypad7", "Key.Keypad7" },
			} };
			for (const auto& [text, canonical] : names)
			{
				CAPTURE(std::string(text));
				const Result<InputBinding> binding = InputBinding::Parse(text);
				REQUIRE(binding.has_value());
				CHECK(binding->ToString() == canonical);
			}

			const Result<InputBinding> axis = InputBinding::Parse("Gamepad.LeftX");
			REQUIRE(axis.has_value());
			CHECK(std::holds_alternative<GamepadAxis>(axis->Target));
			const Result<InputBinding> button = InputBinding::Parse("Gamepad.South");
			REQUIRE(button.has_value());
			CHECK(std::holds_alternative<GamepadButton>(button->Target));

			const std::array<std::string_view, 6> invalid = { "Space", "Key.", "Key.None", "Keyboard.A", "Mouse.Button9", "" };
			for (const std::string_view text : invalid)
			{
				CAPTURE(std::string(text));
				const Result<InputBinding> binding = InputBinding::Parse(text);
				REQUIRE_FALSE(binding.has_value());
				CHECK(binding.error().GetCode() == ErrorCode::Validation);
			}
		}

		TEST_CASE("InputBinding: values outside the enumerations have no name")
		{
			CHECK(InputBinding{ .Target = Key::None }.ToString().empty());
			CHECK(InputBinding{ .Target = static_cast<Key>(33) }.ToString().empty());
			CHECK(InputBinding{ .Target = static_cast<MouseButton>(MouseButtonCount) }.ToString().empty());
			CHECK(InputBinding{ .Target = static_cast<GamepadAxis>(GamepadAxisCount) }.ToString().empty());
			CHECK(InputBinding{ .Target = GamepadAxis::RightTrigger }.ToString() == "Gamepad.RightTrigger");
		}

		TEST_CASE("InputActionMap: a gamepad that disconnects while a bound button is held releases the action")
		{
			const std::vector<InputActionDefinition> definitions = MakeDefinitions();
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE(actions.has_value());
			const std::optional<uint32_t> jump = actions->FindAction("Jump");
			REQUIRE(jump.has_value());

			// Gamepad buttons count on every connected gamepad, not only the first.
			InputState input;
			input.Inject(GamepadEvent{ .Gamepad = 3, .Kind = GamepadEventKind::Connected });
			input.Inject(GamepadEvent{ .Gamepad = 3, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = true });
			input.LatchStep();
			CHECK(actions->IsDown(input, InputPhase::Step, *jump));
			CHECK(actions->WasPressed(input, InputPhase::Step, *jump));

			input.Inject(GamepadEvent{ .Gamepad = 3, .Kind = GamepadEventKind::Disconnected });
			input.LatchStep();
			CHECK_FALSE(actions->IsDown(input, InputPhase::Step, *jump));
			CHECK(actions->WasReleased(input, InputPhase::Step, *jump));
		}

		TEST_CASE("InputActionMap: the button queries of an axis action ignore its gamepad axis")
		{
			const std::vector<InputActionDefinition> definitions = MakeDefinitions();
			const Result<InputActionMap> actions = InputActionMap::Create(definitions);
			REQUIRE(actions.has_value());
			const std::optional<uint32_t> moveX = actions->FindAction("MoveX");
			REQUIRE(moveX.has_value());

			InputState input;
			MoveStick(input, 0, GamepadAxis::LeftX, 1.0f);
			input.LatchFrame();
			CHECK(actions->GetAxis(input, InputPhase::Frame, *moveX) == 1.0f);
			CHECK_FALSE(actions->IsDown(input, InputPhase::Frame, *moveX));
			CHECK_FALSE(actions->WasPressed(input, InputPhase::Frame, *moveX));
		}

		TEST_CASE("InputActionMap: an action index out of range asserts")
		{
			ENGINE_CHECK_DEATH("Platform/InputActionIndexOutOfRange", "Input action index 2 is out of range (2 actions)");
		}
	}

}
