#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Platform/Events.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlayInput.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

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

		TEST_CASE("InputBindings: a real play session serves configured actions by phase and names unknown actions with a hint")
		{
			// The production host (PlaySession::State::GetAction), not the fixture's stand-in.
			Test::ScriptTestFixture compiler;
			const AssetHandle script(470);
			REQUIRE(compiler.AddScript(script, "Assets/Scripts/ActionProbe.luau", R"(local T = {}
function T:OnCreate() self.Log = {} end
function T:OnFixedUpdate()
	table.insert(self.Log, string.format("fixed %d %s %s %s %.2f", Time.GetTick(), tostring(Input.IsActionDown("Jump")),
		tostring(Input.IsActionPressed("Jump")), tostring(Input.IsActionReleased("Jump")), Input.GetAxis("Move")))
end
function T:OnUpdate()
	table.insert(self.Log, string.format("frame %s %s %s", tostring(Input.IsActionDown("Jump")), tostring(Input.IsActionPressed("Jump")),
		tostring(Input.IsActionReleased("Jump"))))
end
return Script.Define("ActionProbe", T))"));
			ScriptSlot component;
			component.Script.SetHandle(script);
			const UUID probe = compiler.GetScene().CreateEntity("Probe").GetUUID();
			compiler.GetScene().FindEntityByID(probe).AddComponent<ScriptComponent>(ScriptComponent{ { component } });
			const auto document = SceneSerializer::ToJson(compiler.GetScene());
			REQUIRE(document.has_value());
			PlaySessionSpecification specification;
			specification.Registry = &compiler.GetTypes();
			specification.Assets = &compiler.GetAssetManager();
			specification.ScriptSchemas = compiler.GetSchemaSnapshot();
			specification.Project.Scripting.PauseOnError = false;
			specification.Project.Input.Actions["Jump"] = {};
			specification.Project.Input.Actions["Move"].Type = InputActionSettings::ActionType::Axis;
			specification.Serial = 1;
			auto session = PlaySession::Create(specification, *document);
			REQUIRE_MESSAGE(session.has_value(), (session ? "" : session.error().ToString()));
			PlayInput& input = (*session)->GetInput();
			REQUIRE(input.Queue(0, { .Type = PlayInputEventType::Action, .Name = "Jump", .State = PlayInputEventState::Down }));
			REQUIRE(input.Queue(0, { .Type = PlayInputEventType::Action, .Name = "Move", .HasValue = true, .Value = 0.5f }));
			REQUIRE(input.Queue(2, { .Type = PlayInputEventType::Action, .Name = "Jump", .State = PlayInputEventState::Up }));
			for (int tick = 0; tick < 3; ++tick)
				(*session)->Tick();
			const auto path = VfsPath::Parse("project://Assets/Tests/ActionProbe.luau");
			REQUIRE(path.has_value());
			const auto log = (*session)->GetScripts()->Evaluate("return table.concat(self.Log, '|')", *path, probe);
			REQUIRE_MESSAGE(log.has_value(), (log ? "" : log.error().ToString()));
			CHECK(log->Value.Get() == Json("fixed 0 true true false 0.50|frame true true false|"
										   "fixed 1 true false false 0.50|frame true false false|"
										   "fixed 2 false false true 0.50|frame false false true"));
			// An unknown name raises the located INPUT_UNKNOWN_ACTION error with the host's did-you-mean hint.
			Test::ExpectLog expected(LogLevel::Error, "INPUT_UNKNOWN_ACTION: no action 'Jmup'");
			const auto unknown = (*session)->GetScripts()->Evaluate("local value = 1\nreturn Input.IsActionDown('Jmup')", *path);
			REQUIRE_FALSE(unknown.has_value());
			CHECK(expected.GetMatchCount() == 1);
			const auto errors = (*session)->GetScriptErrors().GetErrors();
			REQUIRE_FALSE(errors.empty());
			CHECK(errors.back().Line == 2);
			CHECK(errors.back().Script == "Assets/Tests/ActionProbe.luau");
			CHECK(errors.back().Message.find("Input.IsActionDown: ") != std::string::npos);
			CHECK(errors.back().Message.find("Jump") != std::string::npos);
		}
	}

}
