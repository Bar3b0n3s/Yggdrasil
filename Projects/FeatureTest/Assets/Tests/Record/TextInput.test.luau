--!strict
-- The recording suite of Assets/Tests/Replays/TextInput.replay (Architecture §13.6 "Recording suites"; Docs/Decisions/
-- 0033-m22-contract.md decisions 52 and 55): its one case only injects input and reads the scene, so the replay it records
-- with test.run {filter: "Record.TextInput", record} reproduces it without the suite. A click focuses Name; typed text,
-- the editing keys and MaxLength edit it; Enter submits it; Tab moves to Greeting, which End, Left, Delete, typed text
-- and the gamepad's South edit and submit. Re-recording is the one call Scaffold/UiTextInputReplay.jsonl makes.
local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " is in UiTextInput.scene")
	return entity
end

local function Centre(name: string): vector
	local rect = Ui.GetRect(Find(name))
	assert(rect ~= nil, name .. " is laid out")
	return (rect.Min + rect.Max) / 2
end

local function TapKey(key: Key)
	Test.InjectKey(key, "Tap")
	Test.WaitTicks(2)
end

local function Type(text: string)
	Test.InjectText(text)
	Test.WaitTicks(2)
end

return Test.Suite("Record.TextInput", function()
	Test.Case("type into both fields with the editing keys and submit them", function()
		Test.InjectMouse("Left", "Tap", Centre("Name"))
		Test.WaitTicks(2)
		Type("Grace")
		TapKey("Home")
		Type("Ms ")
		TapKey("End")
		TapKey("Backspace")
		Type("e Hopper")
		local name = Find("Name").UiTextInput :: UiTextInput
		Test.ExpectEqual(name.Text, "Ms Grace Hop", "MaxLength keeps the first 12 codepoints")
		TapKey("Enter")
		TapKey("Tab")
		Test.ExpectEqual(Ui.GetFocus(), Find("Greeting"), "Tab moves to Greeting")
		TapKey("End")
		TapKey("Left")
		TapKey("Left")
		TapKey("Delete")
		TapKey("Delete")
		Type("hello")
		Test.InjectGamepad(0, "South", "Tap")
		Test.WaitTicks(2)
		local greeting = Find("Greeting").UiTextInput :: UiTextInput
		Test.ExpectEqual(greeting.Text, "Zoë says hello", "Delete removed hi, and hello was typed at the caret")
		Test.ExpectEqual(greeting.Caret, 14)
	end)
end)
