--!strict
-- The recording suite of Assets/Tests/Replays/UiWorld.replay (Architecture §13.6 "Recording suites"; Docs/Decisions/
-- 0033-m22-contract.md decisions 52 and 55): its one case only injects input and reads the scene, so the replay it records
-- with test.run {filter: "Record.UiWorld", record} reproduces it without the suite. The pointer, placed through the
-- primary camera at canvas pixels of Board, a World canvas (Assets/Scripts/UiWorld/WorldPointer.luau), moves onto Press and
-- clicks it, then presses Level's track and drags it along the canvas's plane with Test.InjectMouseMove before releasing
-- it. Re-recording is the one call Scaffold/UiWorld.jsonl makes.
local WorldPointer = require("../../Scripts/UiWorld/WorldPointer")

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " is in UiWorld.scene")
	return entity
end

return Test.Suite("Record.UiWorld", function()
	Test.Case("click the World canvas's button and drag its slider through the camera", function()
		local board = Find("Board")
		local level = Find("Level")
		local press = WorldPointer.Along(board, Find("Press"), 0.5)
		Test.InjectMouseMove(press)
		Test.WaitTicks(2)
		Test.InjectMouse("Left", "Tap", press)
		Test.WaitTicks(3)
		Test.InjectMouse("Left", "Down", WorldPointer.Along(board, level, 0.1))
		Test.WaitTicks(1)
		for _, along in { 0.25, 0.4, 0.55, 0.7, 0.8 } do
			Test.InjectMouseMove(WorldPointer.Along(board, level, along))
			Test.WaitTicks(1)
		end
		Test.InjectMouse("Left", "Up")
		Test.WaitTicks(2)
		Test.ExpectEqual((level.UiSlider :: UiSlider).Value, 8, "the drag ended at four fifths of the track")
		Test.ExpectEqual(Ui.GetFocus(), Find("Press"), "Press keeps the focus its click took; Level is not Focusable")
	end)
end)
