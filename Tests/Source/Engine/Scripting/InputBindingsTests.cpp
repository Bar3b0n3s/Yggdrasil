#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Platform/Events.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>

#include <vector>

namespace Engine {

	namespace Test {

		static void RunInputNamesCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start().has_value());
			for (size_t i = 0; i < KeyCodeCount; ++i)
			{
				const auto key = static_cast<Key>(i);
				const auto name = KeyToString(key);
				if (name.empty())
					continue;
				CAPTURE(name);
				fixture.Input.Inject(KeyEvent{ .KeyCode = key });
				fixture.Input.LatchStep();
				const auto result = fixture.Evaluate(std::format("return Input.IsKeyDown(\"{}\") and Input.IsKeyPressed(\"{}\")", name, name));
				REQUIRE(result.has_value());
				CHECK(result->Value.Get() == Json(true));
			}
			const auto result = fixture.Evaluate(R"(
for _, button in {"Left", "Right", "Middle", "Button4", "Button5", "Button6", "Button7", "Button8"} do
	assert(not Input.IsMouseButtonDown(button))
	assert(not Input.IsMouseButtonPressed(button))
	assert(not Input.IsMouseButtonReleased(button))
end
for _, button in {"South", "East", "West", "North", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftThumb", "RightThumb", "DPadUp", "DPadRight", "DPadDown", "DPadLeft"} do
	assert(not Input.IsGamepadButtonDown(3, button))
end
for _, axis in {"LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"} do
	assert(Input.GetGamepadAxis(3, axis) == 0)
end
return true
)");
			REQUIRE(result.has_value());
			CHECK(result->Value.Get() == Json(true));
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

		void RunInputBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			RunInputNamesCoverage(mode, snapshots);
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			fixture.Input.Inject(KeyEvent{ .KeyCode = Key::Space });
			fixture.Input.Inject(MouseButtonEvent{});
			fixture.Input.Inject(MouseMoveEvent{ .Position = glm::vec2(10.0f, 20.0f) });
			fixture.Input.Inject(MouseMoveEvent{ .Position = glm::vec2(13.0f, 25.0f) });
			fixture.Input.Inject(MouseScrollEvent{ .Offset = glm::vec2(-2.0f, 4.0f) });
			fixture.Input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Connected });
			fixture.Input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Button, .Button = GamepadButton::South, .Pressed = true });
			fixture.Input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Axis, .Axis = GamepadAxis::LeftY, .Value = 0.75f });
			fixture.Input.LatchStep();
			fixture.Actions.emplace("Move", ScriptActionState{ .Down = true, .Pressed = true, .Released = false, .Axis = -0.5f });
			REQUIRE(fixture.Start().has_value());
			const auto step = fixture.Evaluate(R"(
assert(Input.IsKeyDown("Space") and Input.IsKeyPressed("space") and not Input.IsKeyReleased("SPACE"))
assert(Input.IsMouseButtonDown("Left") and Input.IsMouseButtonPressed("Left") and not Input.IsMouseButtonReleased("Left"))
assert(Input.GetMousePosition() == vector.create(13, 25, 0))
assert(Input.GetMouseDelta() == vector.create(3, 5, 0))
assert(Input.GetScrollDelta() == vector.create(-2, 4, 0))
assert(Input.IsActionDown("Move") and Input.IsActionPressed("Move") and not Input.IsActionReleased("Move"))
assert(Input.GetAxis("Move") == -0.5)
assert(Input.IsGamepadConnected(1) and not Input.IsGamepadConnected(0))
assert(Input.IsGamepadButtonDown(1, "South") and Input.GetGamepadAxis(1, "LeftY") == 0.75)
for _, mode in {"Normal", "Hidden", "Locked"} do
	Input.SetCursorMode(mode)
	assert(Input.GetCursorMode() == mode)
end
return true
)");
			REQUIRE(step.has_value());
			CHECK(step->Value.Get() == Json(true));
			fixture.Input.LatchStep();
			const auto nextStep = fixture.Evaluate("return Input.IsKeyDown('Space') and not Input.IsKeyPressed('Space') and Input.GetMouseDelta() == vector.zero and Input.GetScrollDelta() == vector.zero");
			REQUIRE(nextStep.has_value());
			CHECK(nextStep->Value.Get() == Json(true));
			fixture.Frame.Phase = InputPhase::Frame;
			fixture.Input.LatchFrame();
			const auto frame = fixture.Evaluate("return Input.IsKeyPressed('Space') and Input.IsMouseButtonPressed('Left') and Input.GetMouseDelta() == vector.create(3, 5, 0)");
			REQUIRE(frame.has_value());
			CHECK(frame->Value.Get() == Json(true));
			fixture.Input.Inject(KeyEvent{ .KeyCode = Key::Space, .Action = ButtonAction::Released });
			fixture.Input.Inject(MouseButtonEvent{ .Action = ButtonAction::Released });
			fixture.Input.Inject(GamepadEvent{ .Gamepad = 1, .Kind = GamepadEventKind::Disconnected });
			fixture.Input.LatchFrame();
			const auto released = fixture.Evaluate("return Input.IsKeyReleased('Space') and Input.IsMouseButtonReleased('Left') and not Input.IsKeyDown('Space') and not Input.IsGamepadConnected(1) and not Input.IsGamepadButtonDown(1, 'South') and Input.GetGamepadAxis(1, 'LeftY') == 0");
			REQUIRE(released.has_value());
			CHECK(released->Value.Get() == Json(true));
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
				if (counter.Owner == "Input")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			const auto snapshot = fixture.GetApi().GetCoverage(mode);
			REQUIRE(snapshot);
			snapshots.push_back(*snapshot);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("InputBindings: physical and injected state obey independent phase latches")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunInputBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("InputBindings: every named device value matches the authoritative input table")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunInputNamesCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("InputBindings: invalid names indices and read-only cursor changes are refused")
		{
			for (const bool readOnly : { false, true })
			{
				Test::ScriptTestFixture fixture({ .ReadOnly = readOnly });
				REQUIRE(fixture.Start().has_value());
				const auto result = fixture.Evaluate(R"(
assert(not pcall(Input.IsKeyDown, "None") and not pcall(Input.IsKeyDown, 32))
assert(not pcall(Input.IsMouseButtonDown, "Wrong"))
assert(not pcall(Input.IsGamepadButtonDown, 0, "Wrong"))
assert(not pcall(Input.GetGamepadAxis, 0, "Wrong"))
for _, index in {-1, 4, 0.5, math.huge, 0/0, "0"} do
	assert(not pcall(Input.IsGamepadConnected, index))
	assert(not pcall(Input.IsGamepadButtonDown, index, "South"))
	assert(not pcall(Input.GetGamepadAxis, index, "LeftX"))
end
local ok, message = pcall(Input.GetAxis, "Missing")
assert(not ok and string.find(message, "INPUT_UNKNOWN_ACTION", 1, true))
assert(not pcall(Input.SetCursorMode, "Wrong"))
return Input.GetCursorMode() == "Normal"
)");
				REQUIRE(result.has_value());
				CHECK(result->Value.Get() == Json(true));
				if (readOnly)
				{
					const auto refused = fixture.Evaluate("return not pcall(Input.SetCursorMode, 'Locked')");
					REQUIRE(refused.has_value());
					CHECK(refused->Value.Get() == Json(true));
				}
				CHECK(fixture.Cursor == CursorMode::Normal);
				CHECK(fixture.ExternalMutations.empty());
			}
		}
	}

}
