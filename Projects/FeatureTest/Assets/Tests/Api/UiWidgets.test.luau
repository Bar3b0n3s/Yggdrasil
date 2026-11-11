--!strict
-- The widget proxies and UiWidgets.scene (Architecture §15.6 gates 1, 1b and 4; Docs/Decisions/0033-m22-contract.md
-- decision 36): Scaffold/UiWidgets.jsonl wrote the four widgets with every field non-default somewhere (a disabled button,
-- toggle, slider and scroll view, widgets that are not Focusable and a Focusable scroll view, a Vertical slider, a
-- horizontal scroll view at a scroll position), the canvas's Font, Theme and DefaultFocus, and both RaycastTarget values,
-- so the scene renders through Test.Screenshot in every mode, and every field of UiButton, UiToggle, UiSlider and
-- UiScrollView, and the fields W22.2 added to UiCanvas, UiImage and UiText, read back what the scaffold authored and are
-- written through their proxies, every UiLayoutDirection value included.
local Support = require("./Support")

local INTER = "Assets/Ui/Fonts/Inter-Regular.otf"
local GLYPHS = "Assets/Formats/Fonts/Glyphs.ttf"
local EPSILON = 1e-6
local DIRECTIONS: { UiLayoutDirection } = { "Horizontal", "Vertical" }

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

return Test.Suite("Api.UiWidgets", function()
	Test.Case("the widgets scene renders", function()
		Test.ExpectEqual(Scene.GetName(), "UiWidgets")
		Test.WaitTicks(2)
		Test.Screenshot("UiWidgets")
	end)

	Test.Case("UiButton fields read and write through the proxy", function()
		local play = Support.Find("Play").UiButton :: UiButton
		Test.ExpectEqual(play.Interactable, true, "Play is interactable")
		Test.ExpectEqual(play.Focusable, true, "Play is focusable")
		local quit = Support.Find("Quit").UiButton :: UiButton
		Test.ExpectEqual(Support.RoundTrip(quit, "Interactable", true), false, "the scene's Quit is disabled")
		local hud = Support.Find("Hud").UiButton :: UiButton
		Test.ExpectEqual(Support.RoundTrip(hud, "Focusable", true), false, "the scene's Hud is not focusable")
	end)

	Test.Case("UiToggle fields read and write through the proxy", function()
		local music = Support.Find("Music").UiToggle :: UiToggle
		local check = Support.Find("MusicCheck")
		Test.ExpectEqual(Support.RoundTrip(music, "IsOn", false), true, "the scene's Music is on")
		Test.ExpectEqual(Support.RoundTrip(music, "Graphic", nil), check, "the scene's Music shows MusicCheck")
		Support.RoundTrip(music, "Graphic", check)
		local hints = Support.Find("Hints").UiToggle :: UiToggle
		Test.ExpectEqual(hints.IsOn, false, "the scene's Hints is off")
		Test.ExpectEqual(Support.RoundTrip(hints, "Focusable", true), false, "the scene's Hints is not focusable")
		local locked = Support.Find("Locked").UiToggle :: UiToggle
		Test.ExpectEqual(Support.RoundTrip(locked, "Interactable", true), false, "the scene's Locked is disabled")
	end)

	Test.Case("UiSlider fields read and write through the proxy", function()
		local volume = Support.Find("Volume").UiSlider :: UiSlider
		Test.ExpectEqual(Support.RoundTrip(volume, "Value", 40), 25, "the scene's value")
		Test.ExpectEqual(Support.RoundTrip(volume, "Min", 10), 0, "the scene's Min")
		Test.ExpectEqual(Support.RoundTrip(volume, "Max", 90), 100, "the scene's Max")
		Test.ExpectEqual(Support.RoundTrip(volume, "WholeNumbers", false), true, "the scene's WholeNumbers")
		Test.ExpectEqual(Support.RoundTrip(volume, "Step", 2.5), 5, "the scene's step")
		Test.ExpectEqual(volume.Direction, "Horizontal", "the scene's Volume is horizontal")
		for _, direction in DIRECTIONS do
			Support.RoundTrip(volume, "Direction", direction)
		end
		local fill = Support.Find("VolumeFill")
		local handle = Support.Find("VolumeHandle")
		Test.ExpectEqual(Support.RoundTrip(volume, "Fill", nil), fill, "the scene's fill")
		Support.RoundTrip(volume, "Fill", fill)
		Test.ExpectEqual(Support.RoundTrip(volume, "Handle", nil), handle, "the scene's handle")
		Support.RoundTrip(volume, "Handle", handle)
		local brightness = Support.Find("Brightness").UiSlider :: UiSlider
		Test.ExpectEqual(brightness.Direction, "Vertical", "the scene's Brightness is vertical")
		Test.ExpectEqual(brightness.Min, -1, "Brightness's Min")
		Test.ExpectEqual(brightness.Value, 0.5, "Brightness's value")
		Test.ExpectEqual(Support.RoundTrip(brightness, "Focusable", true), false, "the scene's Brightness is not focusable")
		local dimmer = Support.Find("Dimmer").UiSlider :: UiSlider
		Test.ExpectEqual(Support.RoundTrip(dimmer, "Interactable", true), false, "the scene's Dimmer is disabled")
	end)

	Test.Case("UiScrollView fields read and write through the proxy", function()
		local list = Support.Find("List").UiScrollView :: UiScrollView
		Test.ExpectEqual(list.Vertical, true, "the scene's List scrolls vertically")
		Test.ExpectEqual(list.Horizontal, false, "and not horizontally")
		Test.ExpectEqual(Support.RoundTrip(list, "Focusable", false), true, "the scene's List is focusable")
		Test.ExpectEqual(Support.RoundTrip(list, "ScrollStep", 12), 30, "the scene's scroll step")
		local items = Support.Find("Items")
		Test.ExpectEqual(Support.RoundTrip(list, "Content", nil), items, "the scene's content")
		Support.RoundTrip(list, "Content", items)
		local strip = Support.Find("Strip").UiScrollView :: UiScrollView
		Test.ExpectEqual(Support.RoundTrip(strip, "Interactable", true), false, "the scene's Strip is disabled")
		Test.ExpectEqual(Support.RoundTrip(strip, "Horizontal", false), true, "the scene's Strip scrolls horizontally")
		Test.ExpectEqual(Support.RoundTrip(strip, "Vertical", true), false, "and not vertically")
		Test.ExpectNear(Support.RoundTrip(strip, "ScrollPosition", vector.create(80, 0, 0), EPSILON), vector.create(40, 0, 0), EPSILON)
		Test.ExpectEqual(Support.RoundTrip(strip, "Content", nil), Support.Find("Row"), "the Strip's content")
	end)

	Test.Case("the canvas's Font, Theme and DefaultFocus read and write through the proxy", function()
		local canvas = Support.Find("Menu").UiCanvas :: UiCanvas
		local inter = load(INTER)
		Test.ExpectEqual(Support.RoundTrip(canvas, "Font", nil), inter, "the scene's canvas font")
		Support.RoundTrip(canvas, "Font", load(GLYPHS))
		Test.ExpectEqual(Support.RoundTrip(canvas, "DefaultFocus", Support.Find("Music")), Support.Find("Play"), "the scene's default focus")
		Support.RoundTrip(canvas, "DefaultFocus", nil)
		local theme = canvas.Theme
		Support.ExpectColorNear(theme.Normal, Color.New(0.95, 0.95, 0.95, 1), EPSILON, "the scene's Normal tint")
		Support.ExpectColorNear(theme.Hovered, Color.New(0.8, 0.9, 1, 1), EPSILON, "the scene's Hovered tint")
		Support.ExpectColorNear(theme.Pressed, Color.New(0.55, 0.65, 0.75, 1), EPSILON, "the scene's Pressed tint")
		Support.ExpectColorNear(theme.Focused, Color.New(1, 0.9, 0.6, 1), EPSILON, "the scene's Focused tint")
		Support.ExpectColorNear(theme.Disabled, Color.New(0.45, 0.45, 0.45, 0.6), EPSILON, "the scene's Disabled tint")
		canvas.Theme = {
			Normal = Color.New(1, 1, 1, 1),
			Hovered = Color.New(0.5, 0.5, 0.5, 1),
			Pressed = Color.New(0.25, 0.25, 0.25, 1),
			Focused = Color.New(0, 0.5, 1, 1),
			Disabled = Color.New(0.2, 0.2, 0.2, 0.2),
		}
		local written = canvas.Theme
		Support.ExpectColorNear(written.Normal, Color.New(1, 1, 1, 1), EPSILON, "Theme reads back what was written")
		Support.ExpectColorNear(written.Hovered, Color.New(0.5, 0.5, 0.5, 1), EPSILON, "Theme reads back what was written")
		Support.ExpectColorNear(written.Pressed, Color.New(0.25, 0.25, 0.25, 1), EPSILON, "Theme reads back what was written")
		Support.ExpectColorNear(written.Focused, Color.New(0, 0.5, 1, 1), EPSILON, "Theme reads back what was written")
		Support.ExpectColorNear(written.Disabled, Color.New(0.2, 0.2, 0.2, 0.2), EPSILON, "Theme reads back what was written")
	end)

	Test.Case("RaycastTarget reads and writes through the UiImage and UiText proxies", function()
		local backdrop = Support.Find("Backdrop").UiImage :: UiImage
		Test.ExpectEqual(Support.RoundTrip(backdrop, "RaycastTarget", true), false, "the backdrop lets the pointer through")
		local title = Support.Find("Title").UiText :: UiText
		Test.ExpectEqual(Support.RoundTrip(title, "RaycastTarget", true), false, "the title lets the pointer through")
		local label = Support.Find("PlayLabel").UiText :: UiText
		Test.ExpectEqual(label.RaycastTarget, true, "a button's label catches the pointer for its button")
	end)
end)
