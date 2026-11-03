--!strict
-- UI interaction in every mode (Architecture §15.6 gates 2 and 3; Docs/Decisions/0033-m22-contract.md decisions 22 to 28
-- and 36): injected keys, gamepad buttons and pointer presses drive UiWidgets.scene's widgets through the UiInput phase,
-- every Ui function is called, and every OnUi callback reaches the UiRecorder on the Menu canvas, bubbling from its
-- element. Keyboard and gamepad navigation starts at the canvas's DefaultFocus (Play), and the project's Ui settings add
-- Backspace to Cancel and the right bumper to Next (Scaffold/UiWidgets.jsonl), which a case observes. The suite runs with
-- Isolation "Case", so every case starts from a fresh session with nothing focused. Assets/Tests/Replays/Ui.replay drives
-- the same scene with what Test.Inject* cannot give: the pointer's moves, a slider drag and the wheel.
local RecorderScript = require("../../Scripts/UiWidgets/UiRecorder")

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " is in UiWidgets.scene")
	return entity
end

local function Recorder(): RecorderScript.UiRecorder
	return Find("Menu"):GetScript() :: RecorderScript.UiRecorder
end

-- The laid-out rectangle of `name`, in scene pixels (the view's pixels in a lockstep session).
local function Rect(name: string): UiElementRect
	local rect = Ui.GetRect(Find(name))
	assert(rect ~= nil, name .. " is laid out")
	return rect
end

local function Centre(name: string): vector
	local rect = Rect(name)
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

return Test.Suite("UiWidgets", function()
	Test.Case("keyboard and gamepad navigation start at the canvas's DefaultFocus, and Submit clicks the focused button", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		Test.ExpectEqual(Ui.GetFocus(), nil, "nothing has focus at the start")
		TapKey("Enter")
		Test.ExpectEqual(recorder.Count, cursor, "Submit does nothing while nothing has focus")
		TapKey("Up")
		Test.ExpectEqual(Ui.GetFocus(), Find("Play"), "the first command focuses the canvas's DefaultFocus, whatever its direction")
		TapKey("Enter")
		Test.ExpectEqual(recorder.Clicks.Play, 1, "Submit clicks the focused button")
		TapKey("Right")
		Test.ExpectEqual(Ui.GetFocus(), Find("Music"), "Right moves to the nearest focusable widget to the right")
		TapPad("DPadLeft")
		Test.ExpectEqual(Ui.GetFocus(), Find("Play"), "the gamepad's DPadLeft moves back")
		Test.ExpectEqual(recorder:Since(cursor), "Focus:Play:true,Click:Play,Focus:Play:false,Focus:Music:true,Focus:Music:false,Focus:Play:true")
	end)

	Test.Case("the pointer hovers, presses and clicks, and a press focuses only a focusable widget", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		Test.InjectMouse("Left", "Down", Centre("Play"))
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetHovered(), Find("Play"), "the pointer is over Play")
		Test.ExpectEqual(Ui.GetPressed(), Find("Play"), "the press began on Play")
		Test.Expect(Ui.IsPointerOverUi(), "Play catches the pointer")
		Test.ExpectEqual(Ui.GetFocus(), Find("Play"), "a press focuses a focusable widget")
		Test.InjectMouse("Left", "Up")
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetPressed(), nil, "the release ends the press")
		Test.ExpectEqual(recorder.Clicks.Play, 1, "a press and release over Play click it")
		ClickAt(Centre("Hud"))
		Test.ExpectEqual(recorder.Clicks.Hud, 1, "a button that is not Focusable is clicked")
		Test.ExpectEqual(Ui.GetFocus(), Find("Play"), "without taking focus")
		ClickAt(Centre("Quit"))
		Test.Expect(Ui.IsPointerOverUi(), "a disabled button catches the pointer")
		Test.ExpectEqual(Ui.GetHovered(), nil, "but is never hovered")
		Test.ExpectEqual(recorder.Clicks.Quit, nil, "nor clicked")
		ClickAt(Centre("Title"))
		Test.Expect(not Ui.IsPointerOverUi(), "the title and the backdrop let the pointer through")
		Test.ExpectEqual(recorder:Since(cursor), "Hover:Play:true,Focus:Play:true,Click:Play,Hover:Play:false,Hover:Hud:true,Click:Hud,Hover:Hud:false")
	end)

	Test.Case("Submit flips a focused toggle, and the directions along a focused slider step it", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		local music = Find("Music").UiToggle :: UiToggle
		Ui.SetFocus(Find("Music"))
		TapKey("Space")
		Test.ExpectEqual(music.IsOn, false, "Space submits the focused toggle")
		TapPad("South")
		Test.ExpectEqual(music.IsOn, true, "and so does the gamepad's South")
		local volume = Find("Volume").UiSlider :: UiSlider
		Ui.SetFocus(Find("Volume"))
		TapKey("Right")
		Test.ExpectEqual(volume.Value, 30, "Right steps a horizontal slider by its Step")
		TapPad("DPadLeft")
		TapPad("DPadLeft")
		Test.ExpectEqual(volume.Value, 20, "DPadLeft steps it back")
		TapKey("Down")
		Test.ExpectEqual(Ui.GetFocus(), Find("Item1"), "a direction across the slider navigates")
		Test.ExpectEqual(recorder.Values.Music, true, "the toggle's last value")
		Test.ExpectEqual(recorder.Values.Volume, 20, "the slider's last value")
		Test.ExpectEqual(recorder:Since(cursor),
			"Focus:Music:true,Value:Music:false,Value:Music:true,Focus:Music:false,Focus:Volume:true,Value:Volume:30,Value:Volume:25,"
				.. "Value:Volume:20,Focus:Volume:false,Focus:Item1:true")
	end)

	Test.Case("a pointer press sets a slider from the pointer, and one that is not Focusable leaves focus as it is", function()
		local recorder = Recorder()
		local brightness = Find("Brightness").UiSlider :: UiSlider
		local rect = Rect("Brightness")
		-- Seven eighths of the way up from the bottom edge, its Min: -1 + 2 * 0.875.
		ClickAt(vector.create((rect.Min.x + rect.Max.x) / 2, rect.Max.y - (rect.Max.y - rect.Min.y) * 0.875, 0))
		Test.ExpectNear(brightness.Value, 0.75, 1e-5)
		Test.ExpectNear(recorder.Values.Brightness :: number, 0.75, 1e-5)
		Test.ExpectEqual(Ui.GetFocus(), nil, "Brightness is not Focusable")
	end)

	Test.Case("focus inside a scroll view's content scrolls it into view, and a focused scroll view steps along its axis", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		local list = Find("List").UiScrollView :: UiScrollView
		Test.ExpectEqual(list.ScrollPosition, vector.zero, "the list starts at its top")
		Ui.SetFocus(Find("Item6"))
		-- Item6 spans 250 to 300 units below the content's top, and the view is 200 units high.
		Test.ExpectEqual(list.ScrollPosition, vector.create(0, 100, 0), "focusing Item6 scrolls it to the view's bottom edge at once")
		Test.WaitTicks(1)
		Ui.SetFocus(Find("List"))
		TapKey("Down")
		Test.ExpectEqual(list.ScrollPosition, vector.create(0, 130, 0), "Down steps the focused list by its ScrollStep")
		TapPad("DPadUp")
		Test.ExpectEqual(list.ScrollPosition, vector.create(0, 100, 0), "and DPadUp steps it back")
		Test.ExpectEqual(recorder.Scrolls.List, vector.create(0, 100, 0), "the list's last scroll position")
		Test.ExpectEqual(recorder:Since(cursor), "Focus:Item6:true,Scroll:List:0,100,Focus:Item6:false,Focus:List:true,Scroll:List:0,130,Scroll:List:0,100")
	end)

	Test.Case("Cancel reaches the focused element, else the last canvas", function()
		local recorder = Recorder()
		TapKey("Escape")
		Test.ExpectEqual(recorder.Cancels.Menu, 1, "with nothing focused, Cancel reaches the canvas")
		Ui.SetFocus(Find("Play"))
		TapPad("East")
		Test.ExpectEqual(recorder.Cancels.Play, 1, "the gamepad's East cancels at the focused button")
	end)

	Test.Case("the project's Ui bindings add Backspace to Cancel and the right bumper to Next, and Shift turns Next into Previous", function()
		local recorder = Recorder()
		Ui.SetFocus(Find("Play"))
		TapPad("RightBumper")
		Test.ExpectEqual(Ui.GetFocus(), Find("Music"), "Next is the following focusable widget in draw order")
		TapKey("Backspace")
		Test.ExpectEqual(recorder.Cancels.Music, 1, "Backspace cancels")
		TapKey("Tab")
		Test.ExpectEqual(Ui.GetFocus(), Find("Volume"), "Tab is Next")
		Test.InjectKey("LeftShift", "Down")
		TapKey("Tab")
		Test.InjectKey("LeftShift", "Up")
		Test.WaitTicks(1)
		Test.ExpectEqual(Ui.GetFocus(), Find("Music"), "Shift and Tab is Previous")
	end)

	Test.Case("Ui.SetFocus refuses an element that cannot take focus, nil clears focus, and GetRect reads the layout", function()
		local recorder = Recorder()
		local cursor = recorder.Count
		for _, name in { "Quit", "Hud", "Hints", "Locked", "Dimmer", "Brightness", "Strip", "Title" } do
			Test.Expect(not pcall(Ui.SetFocus, Find(name)), `{name} cannot take focus`)
		end
		Test.ExpectEqual(Ui.GetFocus(), nil, "a refused element changes nothing")
		Ui.SetFocus(Find("Item1"))
		Test.ExpectEqual(Ui.GetFocus(), Find("Item1"), "focus changes at once")
		Ui.SetFocus(nil)
		Test.ExpectEqual(Ui.GetFocus(), nil, "nil clears focus")
		Test.WaitTicks(1)
		Test.ExpectEqual(recorder:Since(cursor), "Focus:Item1:true,Focus:Item1:false")
		local play = Rect("Play")
		Test.ExpectEqual(play.Min, vector.create(40, 100, 0), "Play's top-left corner in scene pixels")
		Test.ExpectEqual(play.Max, vector.create(280, 156, 0), "Play's bottom-right corner in scene pixels")
		Test.ExpectEqual(Ui.GetRect(Find("Camera")), nil, "an entity that is no UI element has no rectangle")
	end)
end)
