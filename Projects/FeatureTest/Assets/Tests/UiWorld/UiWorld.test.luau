--!strict
-- A World canvas's pointer in every mode (Docs/Decisions/0033-m22-contract.md decisions 50 and 55): UiWorld.scene's Board
-- is a World canvas turned away from the primary camera, and the pointer, placed through the camera at a canvas pixel
-- (Assets/Scripts/UiWorld/WorldPointer.luau), hovers, presses and clicks its button and drags its slider along its plane,
-- the UiInput phase casting the pointer's ray onto the canvas. The suite runs with Isolation "Case", so every case starts
-- from a fresh session with nothing focused.
local RecorderScript = require("../../Scripts/UiWorld/WorldRecorder")
local WorldPointer = require("../../Scripts/UiWorld/WorldPointer")

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " is in UiWorld.scene")
	return entity
end

local function Recorder(): RecorderScript.WorldRecorder
	return Find("Board"):GetScript() :: RecorderScript.WorldRecorder
end

return Test.Suite("UiWorld", function()
	Test.Case("the pointer hovers, presses and clicks a World canvas's button through the camera", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		local press = WorldPointer.Along(Find("Board"), Find("Press"), 0.5)
		Test.InjectMouseMove(press)
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetHovered(), Find("Press"), "the ray through the pointer meets Press on the canvas")
		Test.Expect(Ui.IsPointerOverUi(), "the World canvas catches the pointer")
		Test.InjectMouse("Left", "Tap", press)
		Test.WaitTicks(2)
		Test.ExpectEqual(recorder.Clicks.Press, 1, "a press and release over Press click it")
		Test.ExpectEqual(Ui.GetFocus(), Find("Press"), "and the press focused it")
		Test.InjectMouseMove(vector.create(4, 4, 0))
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetHovered(), nil, "a pointer beside the canvas hovers nothing")
		Test.Expect(not Ui.IsPointerOverUi(), "and is over no UI")
		Test.ExpectEqual(recorder:Since(cursor), "Hover:Press:true,Focus:Press:true,Click:Press,Hover:Press:false")
	end)

	Test.Case("a World canvas's slider takes the pointer's value and drags along its plane", function()
		local recorder = Recorder()
		local board = Find("Board")
		local level = Find("Level")
		local slider = level.UiSlider :: UiSlider
		Test.InjectMouse("Left", "Down", WorldPointer.Along(board, level, 0.2))
		Test.WaitTicks(1)
		Test.ExpectEqual(slider.Value, 2, "a press sets the value at the pointer, a fifth of the way along")
		Test.InjectMouseMove(WorldPointer.Along(board, level, 0.5))
		Test.WaitTicks(1)
		Test.ExpectEqual(slider.Value, 5, "the drag follows the pointer along the canvas's plane")
		Test.InjectMouseMove(WorldPointer.Along(board, level, 0.9))
		Test.WaitTicks(1)
		Test.ExpectEqual(slider.Value, 9)
		Test.InjectMouse("Left", "Up")
		Test.InjectMouseMove(WorldPointer.Along(board, level, 0.3))
		Test.WaitTicks(2)
		Test.ExpectEqual(slider.Value, 9, "after the release the pointer no longer drags it")
		Test.ExpectEqual(recorder.Values.Level, 9, "the slider's last value")
	end)
end)
