--!strict
-- The Input module without input (§11.5): every query reads idle for every mouse button, gamepad button and axis, the
-- project's actions are known by name, and the cursor mode takes every value. The Input suite drives input with
-- Test.Inject*, and Input.replay plays recorded input.
local MOUSE_BUTTONS: { MouseButton } = { "Left", "Right", "Middle", "Button4", "Button5", "Button6", "Button7", "Button8" }
local GAMEPAD_BUTTONS: { GamepadButton } = {
	"South", "East", "West", "North", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftThumb", "RightThumb",
	"DPadUp", "DPadRight", "DPadDown", "DPadLeft",
}
local GAMEPAD_AXES: { GamepadAxis } = { "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger" }
local KEYS: { Key } = { "Space", "A", "D", "W", "S", "Escape", "Enter", "LeftShift", "F1", "Keypad0" }

return Test.Suite("Api.Input", function()
	Test.Case("without input every key, button, axis and action reads idle", function()
		for _, key in KEYS do
			Test.Expect(not Input.IsKeyDown(key) and not Input.IsKeyPressed(key) and not Input.IsKeyReleased(key), key)
		end
		for _, button in MOUSE_BUTTONS do
			Test.Expect(not Input.IsMouseButtonDown(button), button .. " down")
			Test.Expect(not Input.IsMouseButtonPressed(button), button .. " pressed")
			Test.Expect(not Input.IsMouseButtonReleased(button), button .. " released")
		end
		for index = 0, 3 do
			Test.Expect(not Input.IsGamepadConnected(index), "gamepad " .. index)
			for _, button in GAMEPAD_BUTTONS do
				Test.Expect(not Input.IsGamepadButtonDown(index, button), button)
			end
			for _, axis in GAMEPAD_AXES do
				Test.ExpectEqual(Input.GetGamepadAxis(index, axis), 0, axis)
			end
		end
		for _, action in { "Jump", "Fire", "MoveX", "MoveY" } do
			Test.Expect(not Input.IsActionDown(action) and not Input.IsActionPressed(action) and not Input.IsActionReleased(action), action)
			Test.ExpectEqual(Input.GetAxis(action), 0, action)
		end
		Test.ExpectEqual(Input.GetMouseDelta(), vector.zero)
		Test.ExpectEqual(Input.GetScrollDelta(), vector.zero)
		local position = Input.GetMousePosition()
		Test.ExpectEqual(position.z, 0, "a view position has no depth")
		-- Built at run time: project.validate reports literal unknown action names (INPUT_UNKNOWN_ACTION).
		local undeclared = table.concat({ "Not", "Declared" })
		Test.Expect(not pcall(Input.IsActionDown, undeclared), "an unknown action is an error")
		Test.Expect(not pcall(Input.IsKeyDown, "NotAKey" :: any), "an unknown key is an error")
	end)

	Test.Case("the cursor mode takes every value", function()
		Test.ExpectEqual(Input.GetCursorMode(), "Normal")
		for _, mode: CursorMode in { "Locked", "Hidden", "Normal" } do
			Input.SetCursorMode(mode)
			Test.ExpectEqual(Input.GetCursorMode(), mode)
		end
	end)
end)
