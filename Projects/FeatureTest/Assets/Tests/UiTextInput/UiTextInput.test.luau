--!strict
-- Text input in every mode (Architecture §15.6 gates 2 and 3; Docs/Decisions/0033-m22-contract.md decisions 46, 47, 52
-- and 55): injected clicks, text, editing keys, gamepad buttons, pointer moves and the wheel drive UiTextInput.scene
-- through the TextLatch and UiInput phases; Input.GetText reads the tick's committed characters; OnUiTextChanged and
-- OnUiSubmit reach the TextRecorder on the Form canvas, bubbling from their input with its text; and Test.InjectText,
-- Test.InjectMouseMove and Test.InjectScroll queue what a recording suite needs (Assets/Tests/Record/TextInput.test.luau
-- records Assets/Tests/Replays/TextInput.replay from the same functions). The suite runs with Isolation "Case", so every
-- case starts from a fresh session with nothing focused.
local RecorderScript = require("../../Scripts/UiTextInput/TextRecorder")

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " is in UiTextInput.scene")
	return entity
end

local function Recorder(): RecorderScript.TextRecorder
	return Find("Form"):GetScript() :: RecorderScript.TextRecorder
end

local function Field(name: string): UiTextInput
	return Find(name).UiTextInput :: UiTextInput
end

-- The centre of `name`'s laid-out rectangle, in scene pixels (the view's pixels in a lockstep session).
local function Centre(name: string): vector
	local rect = Ui.GetRect(Find(name))
	assert(rect ~= nil, name .. " is laid out")
	return (rect.Min + rect.Max) / 2
end

-- A tap applies at the next tick and releases at the one after; each helper resumes once both have run.
local function TapKey(key: Key)
	Test.InjectKey(key, "Tap")
	Test.WaitTicks(2)
end

local function TapPad(button: GamepadButton)
	Test.InjectGamepad(0, button, "Tap")
	Test.WaitTicks(2)
end

local function ClickAt(position: vector)
	Test.InjectMouse("Left", "Tap", position)
	Test.WaitTicks(2)
end

-- Text applies at the next tick; the case resumes in that tick's Tasks phase, after the UI events and OnFixedUpdate.
local function Type(text: string)
	Test.InjectText(text)
	Test.WaitTicks(1)
end

return Test.Suite("UiTextInput", function()
	Test.Case("a click focuses a text input, and typed text inserts at the caret and reaches Input.GetText", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		local name = Field("Name")
		ClickAt(Centre("Name"))
		Test.ExpectEqual(Ui.GetFocus(), Find("Name"), "a press focuses the text input")
		Test.ExpectEqual(Input.GetText(), "", "no text was typed in this tick")
		Type("Ada")
		Test.ExpectEqual(Input.GetText(), "Ada", "the tick's committed characters")
		Test.ExpectEqual(name.Text, "Ada", "typing inserts at the caret")
		Test.ExpectEqual(name.Caret, 3, "the caret follows the inserted codepoints")
		Test.WaitTicks(1)
		Test.ExpectEqual(Input.GetText(), "", "a tick without text reads none")
		Test.ExpectEqual(recorder.Changes.Name, 1, "one change")
		Test.ExpectEqual(recorder:Since(cursor), "Hover:Name:true,Focus:Name:true,Changed:Name:Ada,Typed:Ada")
	end)

	Test.Case("the editing keys remove and move by codepoint, and Enter submits the text", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		local greeting = Field("Greeting")
		Ui.SetFocus(Find("Greeting"))
		TapKey("Backspace")
		Test.ExpectEqual(greeting.Text, "Zo says hi", "Backspace removes the codepoint before the caret, ë's two bytes")
		Test.ExpectEqual(greeting.Caret, 2)
		Type("é")
		Test.ExpectEqual(greeting.Text, "Zoé says hi", "typing inserts at the caret")
		TapKey("End")
		Test.ExpectEqual(greeting.Caret, 11, "End moves the caret past the last of the 11 codepoints")
		Type("!")
		TapKey("Home")
		TapKey("Right")
		TapKey("Right")
		Test.ExpectEqual(greeting.Caret, 2, "Home, then Right twice")
		TapKey("Delete")
		Test.ExpectEqual(greeting.Text, "Zo says hi!", "Delete removes the codepoint after the caret")
		TapKey("Left")
		Test.ExpectEqual(greeting.Caret, 1, "Left moves the caret back one codepoint")
		TapKey("Enter")
		Test.ExpectEqual(recorder.Submits.Greeting, "Zo says hi!", "Enter submits the text")
		Test.ExpectEqual(Ui.GetFocus(), Find("Greeting"), "the input keeps focus")
		Test.ExpectEqual(recorder:Since(cursor),
			"Focus:Greeting:true,Changed:Greeting:Zo says hi,Changed:Greeting:Zoé says hi,Typed:é,Changed:Greeting:Zoé says hi!,Typed:!,"
				.. "Changed:Greeting:Zo says hi!,Submit:Greeting:Zo says hi!")
	end)

	Test.Case("MaxLength drops what is typed past it, and a disabled or unfocusable input takes no text", function()
		local name = Field("Name")
		ClickAt(Centre("Name"))
		Type("Ada Lovelace Byron")
		Test.ExpectEqual(name.Text, "Ada Lovelace", "the first 12 codepoints, Name's MaxLength")
		ClickAt(Centre("Locked"))
		Test.ExpectEqual(Ui.GetFocus(), Find("Name"), "a disabled input takes no focus")
		Ui.SetFocus(nil)
		ClickAt(Centre("Code"))
		Test.ExpectEqual(Ui.GetFocus(), nil, "an input that is not Focusable takes no focus")
		Type("x")
		Test.ExpectEqual(Field("Code").Text, "", "text typed with nothing focused reaches no input")
		Test.ExpectEqual(Field("Locked").Text, "Read only")
		Test.ExpectEqual(name.Text, "Ada Lovelace")
	end)

	Test.Case("keyboard navigation pauses while a text input has focus; Next and the gamepad still navigate, and South submits", function()
		local recorder = Recorder()
		Ui.SetFocus(Find("Name"))
		TapKey("Down")
		TapKey("Space")
		Test.ExpectEqual(Ui.GetFocus(), Find("Name"), "a keyboard direction does not navigate away from a text input")
		Test.ExpectEqual(recorder.Submits.Name, nil, "nor does Space submit it")
		TapPad("South")
		Test.ExpectEqual(recorder.Submits.Name, "", "the gamepad's South submits it")
		TapKey("Tab")
		Test.ExpectEqual(Ui.GetFocus(), Find("Greeting"), "Tab is Next")
		TapPad("DPadDown")
		Test.ExpectEqual(Ui.GetFocus(), Find("Send"), "the gamepad's DPadDown navigates")
		TapKey("Up")
		Test.ExpectEqual(Ui.GetFocus(), Find("Greeting"), "away from a text input the keyboard navigates again")
	end)

	Test.Case("the pointer's moves hover widgets, and the wheel scrolls the list under the pointer", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		Test.InjectMouseMove(Centre("Send"))
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetHovered(), Find("Send"), "a pointer move hovers the button")
		Test.InjectMouseMove(Centre("Log"))
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetHovered(), Find("Log"), "and then the list")
		Test.InjectScroll(vector.create(0, -2, 0))
		Test.WaitTicks(1)
		local log = Find("Log").UiScrollView :: UiScrollView
		Test.ExpectEqual(log.ScrollPosition, vector.create(0, 80, 0), "two wheel steps down scroll the list by twice its ScrollStep")
		Test.ExpectEqual(recorder:Since(cursor), "Hover:Send:true,Hover:Send:false,Hover:Log:true,Scroll:Log:0,80")
	end)
end)
