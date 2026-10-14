--!strict
-- Input through Test.Inject* (§11.5, §15.5): keys, mouse buttons and the cursor, gamepads with the engine's
-- up-positive stick Y, the project's actions and axes (MoveY inverts its stick), and phase-aware edges: an event applied
-- at a step is seen as one press or release edge by that step and once more by the following frame. Injected input
-- applies at the next tick. Mouse movement, the wheel and text input are exercised by Assets/Tests/Replays/Input.replay.
local ProbeScript = require("../../Scripts/Input/InputProbe")

local MOUSE_BUTTONS: { MouseButton } = { "Left", "Right", "Middle", "Button4", "Button5", "Button6", "Button7", "Button8" }
local GAMEPAD_BUTTONS: { GamepadButton } = {
	"South", "East", "West", "North", "LeftBumper", "RightBumper", "Back", "Start", "Guide", "LeftThumb", "RightThumb",
	"DPadUp", "DPadRight", "DPadDown", "DPadLeft",
}
local GAMEPAD_AXES: { GamepadAxis } = { "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger" }
local DEAD_ZONE = 0.15

local function F32(value: number): number
	local scratch = buffer.create(4)
	buffer.writef32(scratch, 0, value)
	return buffer.readf32(scratch, 0)
end

-- InputActionMap::ApplyDeadZone (§4.3).
local function DeadZone(value: number): number
	local magnitude = math.min(math.abs(value), 1)
	if magnitude <= DEAD_ZONE then
		return 0
	end
	return math.sign(value) * (magnitude - DEAD_ZONE) / (1 - DEAD_ZONE)
end

local function Probe(): ProbeScript.InputProbe
	local entity = Scene.FindByName("InputProbe")
	assert(entity ~= nil, "Input.scene has an InputProbe")
	return entity:GetScript() :: ProbeScript.InputProbe
end

local function Step()
	Test.WaitTicks(1)
end

return Test.Suite("Input", function()
	Test.Case("a key press and release are each one edge in the step and one in the next frame", function()
		local probe = Probe()
		local steps, frames = probe.StepCount + 1, probe.FrameCount + 1
		Test.InjectKey("Space", "Down")
		Test.Expect(not Input.IsKeyDown("Space"), "injected input waits for the next tick")
		Step()
		Test.Expect(Input.IsKeyDown("Space") and Input.IsKeyPressed("Space") and not Input.IsKeyReleased("Space"), "pressed this step")
		Test.Expect(Input.IsActionDown("Jump") and Input.IsActionPressed("Jump"), "Jump is bound to Space")
		Step()
		Test.Expect(Input.IsKeyDown("Space") and not Input.IsKeyPressed("Space"), "held without a second edge")
		Test.InjectKey("Space", "Up")
		Step()
		Test.Expect(not Input.IsKeyDown("Space") and Input.IsKeyReleased("Space"), "released this step")
		Test.Expect(Input.IsActionReleased("Jump") and not Input.IsActionDown("Jump"), "Jump follows Space")
		Step()
		Test.Expect(not Input.IsKeyReleased("Space"), "the release edge lasts one step")
		Test.InjectKey("Space", "Tap")
		Step()
		Test.Expect(Input.IsKeyPressed("Space") and Input.IsKeyDown("Space"), "a tap presses on its tick")
		Step()
		Test.Expect(Input.IsKeyReleased("Space") and not Input.IsKeyDown("Space"), "and releases on the next")
		Step()
		-- Lockstep runs one frame after every step, and each frame sees exactly the edges of the step before it.
		for _, phase in { "Steps", "Frames" } do
			local first = if phase == "Steps" then steps else frames
			Test.ExpectEqual(probe:Count(phase, "SpacePressed", first), 2, phase .. ": two presses")
			Test.ExpectEqual(probe:Count(phase, "SpaceReleased", first), 2, phase .. ": two releases")
			Test.ExpectEqual(probe:Count(phase, "JumpPressed", first), 2, phase .. ": two Jump presses")
			Test.ExpectEqual(probe:Count(phase, "JumpReleased", first), 2, phase .. ": two Jump releases")
		end
		local released = probe:StepAt(Time.GetTick() - 1)
		Test.Expect(released ~= nil and released.SpaceReleased and not released.SpaceDown, "the previous step saw the tap's release")
		local frame = probe:LastFrame()
		Test.Expect(frame.SpaceReleased and frame.Tick == Time.GetTick(), "and so did the frame that followed it")
	end)

	Test.Case("keys drive the MoveX and MoveY axes from both directions", function()
		Test.InjectKey("D", "Down")
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), 1)
		Test.ExpectEqual(Probe():LastStep().DDown, true)
		Test.InjectKey("A", "Down")
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), 0, "opposite keys cancel")
		Test.InjectKey("D", "Up")
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), -1)
		Test.InjectKey("A", "Up")
		Test.InjectKey("W", "Down")
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), 0)
		Test.ExpectEqual(Input.GetAxis("MoveY"), 1, "Invert flips only MoveY's gamepad axis, not its keys")
		Test.InjectKey("W", "Up")
		Test.InjectKey("Down", "Tap")
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveY"), -1)
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveY"), 0)
	end)

	Test.Case("every mouse button reports down, pressed and released, and the cursor follows injected positions", function()
		for index, button in MOUSE_BUTTONS do
			Test.InjectMouse(button, "Tap", vector.create(100 + index * 10, 50 + index, 0))
		end
		Step()
		for _, button in MOUSE_BUTTONS do
			Test.Expect(Input.IsMouseButtonDown(button) and Input.IsMouseButtonPressed(button), button .. " pressed")
			Test.Expect(not Input.IsMouseButtonReleased(button), button .. " not released yet")
		end
		Test.ExpectEqual(Input.GetMousePosition(), vector.create(180, 58, 0), "the last injected position")
		Test.Expect(Input.IsActionPressed("Fire"), "Fire is bound to the left button")
		Step()
		for _, button in MOUSE_BUTTONS do
			Test.Expect(Input.IsMouseButtonReleased(button) and not Input.IsMouseButtonDown(button), button .. " released")
			Test.Expect(not Input.IsMouseButtonPressed(button), button .. " not pressed again")
		end
		Test.InjectMouse("Right", "Down")
		Step()
		Test.Expect(Input.IsMouseButtonDown("Right"), "Right held")
		Test.ExpectEqual(Input.GetMousePosition(), vector.create(180, 58, 0), "a button without a position keeps the cursor")
		Test.InjectMouse("Right", "Up", vector.create(640, 360, 0))
		Step()
		Test.Expect(Input.IsMouseButtonReleased("Right"), "Right released")
		Test.ExpectEqual(Input.GetMousePosition(), vector.create(640, 360, 0))
		-- Test.Inject* cannot move the wheel; Input.replay does.
		Test.ExpectEqual(Input.GetScrollDelta(), vector.zero)
		Step()
		Test.ExpectEqual(Input.GetMouseDelta(), vector.zero, "the cursor did not move this step")
	end)

	Test.Case("the cursor mode takes every value", function()
		Test.ExpectEqual(Input.GetCursorMode(), "Normal", "each test session starts with the Normal cursor")
		for _, mode: CursorMode in { "Hidden", "Locked", "Normal" } do
			Input.SetCursorMode(mode)
			Test.ExpectEqual(Input.GetCursorMode(), mode)
		end
	end)

	Test.Case("every gamepad button reports its state on the injected pad only", function()
		Test.Expect(not Input.IsGamepadConnected(0), "no pad before input")
		for _, button in GAMEPAD_BUTTONS do
			Test.InjectGamepad(0, button, "Down")
		end
		Step()
		Test.Expect(Input.IsGamepadConnected(0), "input connects the pad")
		Test.Expect(not Input.IsGamepadConnected(1), "and only that pad")
		for _, button in GAMEPAD_BUTTONS do
			Test.Expect(Input.IsGamepadButtonDown(0, button), button .. " down")
			Test.Expect(not Input.IsGamepadButtonDown(1, button), button .. " up on pad 1")
		end
		Test.Expect(Input.IsActionDown("Jump") and Input.IsActionDown("Fire"), "South drives Jump and RightBumper Fire")
		for _, button in GAMEPAD_BUTTONS do
			Test.InjectGamepad(0, button, "Up")
		end
		Step()
		for _, button in GAMEPAD_BUTTONS do
			Test.Expect(not Input.IsGamepadButtonDown(0, button), button .. " up")
		end
		Test.InjectGamepad(0, "North", "Tap")
		Step()
		Test.Expect(Input.IsGamepadButtonDown(0, "North"), "a tap holds for its tick")
		Step()
		Test.Expect(not Input.IsGamepadButtonDown(0, "North"), "and releases on the next")
	end)

	Test.Case("gamepad axes use the engine convention and actions apply the dead zone and inversion", function()
		local values = { 0.6, 0.8, -0.35, -1, 0.5, 1 } -- in GAMEPAD_AXES order
		for index, axis in GAMEPAD_AXES do
			Test.InjectGamepad(0, axis, values[index])
		end
		Step()
		for index, axis in GAMEPAD_AXES do
			Test.ExpectNear(Input.GetGamepadAxis(0, axis), F32(values[index]), 1e-7, axis .. " reads the injected value")
			Test.ExpectEqual(Input.GetGamepadAxis(2, axis), 0, axis .. " is zero on a disconnected pad")
		end
		Test.ExpectNear(Input.GetAxis("MoveX"), DeadZone(0.6), 1e-6, "MoveX applies the dead zone to LeftX")
		-- Pushing the stick up is positive LeftY; MoveY inverts its stick, so up moves MoveY negative.
		Test.ExpectNear(Input.GetAxis("MoveY"), -DeadZone(0.8), 1e-6, "MoveY inverts LeftY")
		Test.InjectGamepad(0, "LeftX", 0.1)
		Test.InjectGamepad(0, "LeftY", -0.4)
		Step()
		Test.ExpectNear(Input.GetGamepadAxis(0, "LeftX"), F32(0.1), 1e-7)
		Test.ExpectEqual(Input.GetAxis("MoveX"), 0, "inside the dead zone")
		Test.ExpectNear(Input.GetAxis("MoveY"), DeadZone(0.4), 1e-6, "pulling the stick down moves MoveY positive")
		local probe = Probe()
		local reading = probe:LastStep()
		Test.ExpectNear(reading.LeftY, F32(-0.4), 1e-7)
		Test.ExpectNear(reading.RightTrigger, 1, 1e-7)
		for _, axis in GAMEPAD_AXES do
			Test.InjectGamepad(0, axis, 0)
		end
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), 0)
		Test.ExpectEqual(Input.GetAxis("MoveY"), 0)
	end)

	Test.Case("actions take injected button states and analog values", function()
		Test.InjectAction("Jump", "Tap")
		Step()
		Test.Expect(Input.IsActionPressed("Jump") and Input.IsActionDown("Jump"), "Jump tap pressed")
		Step()
		Test.Expect(Input.IsActionReleased("Jump") and not Input.IsActionDown("Jump"), "Jump tap released")
		Test.InjectAction("Fire", "Down")
		Step()
		Test.Expect(Input.IsActionDown("Fire") and Input.IsActionPressed("Fire"), "Fire held")
		Test.Expect(not Input.IsMouseButtonDown("Left"), "an action is not its bindings")
		Test.InjectAction("Fire", "Up")
		Step()
		Test.Expect(Input.IsActionReleased("Fire") and not Input.IsActionDown("Fire"), "Fire released")
		Test.InjectAction("MoveX", nil, 0.25)
		Test.InjectAction("MoveY", "Down", -0.5)
		Step()
		Test.ExpectNear(Input.GetAxis("MoveX"), 0.25, 1e-7)
		Test.ExpectNear(Input.GetAxis("MoveY"), -0.5, 1e-7)
		Step()
		local probe = Probe()
		local frame = probe:LastFrame()
		Test.ExpectNear(frame.MoveX, 0.25, 1e-7, "the frame after the step saw the analog value too")
		Test.ExpectNear(frame.MoveY, -0.5, 1e-7)
		Test.ExpectNear(Input.GetAxis("MoveX"), 0.25, 1e-7, "an analog value holds until it changes")
		Test.InjectAction("MoveX", nil, 0)
		Test.InjectAction("MoveY", nil, 0)
		Step()
		Test.ExpectEqual(Input.GetAxis("MoveX"), 0)
		Test.ExpectEqual(Input.GetAxis("MoveY"), 0)
	end)
end)
