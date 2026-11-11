--!strict
-- UiCanvas's World fields and UiWorld.scene (Architecture §15.6 gates 1, 1b and 4; Docs/Decisions/0033-m22-contract.md
-- decisions 49, 51 and 55): Scaffold/UiWorld.jsonl wrote Board, a World canvas with every World field non-default (its
-- RenderMode, WorldWidth, WorldHeight and PixelsPerUnit) turned in the world, beside Hud, a Screen canvas, so the scene
-- renders through Test.Screenshot in every mode (the canvas depth-tested in the world, the HUD over it); Ui.GetRect reports
-- a World canvas's element in that canvas's pixels and a Screen canvas's in scene pixels, each with its canvas; and the four
-- fields read back what the scaffold authored and are written through the proxy, both UiCanvasRenderMode values included.
local Support = require("./Support")

local EPSILON = 1e-6

return Test.Suite("Api.UiWorld", function()
	Test.Case("the world canvas scene renders", function()
		Test.ExpectEqual(Scene.GetName(), "UiWorld")
		Test.WaitTicks(2)
		Test.Screenshot("UiWorld")
	end)

	Test.Case("Ui.GetRect reports a World canvas's element in its canvas pixels and a Screen canvas's in scene pixels, with their canvases", function()
		local press = Ui.GetRect(Support.Find("Press"))
		assert(press ~= nil, "Press is laid out")
		Test.ExpectEqual(press.Min, vector.create(40, 120, 0), "Press's top-left corner in Board's pixels")
		Test.ExpectEqual(press.Max, vector.create(360, 232, 0), "Press's bottom-right corner in Board's pixels")
		Test.ExpectEqual(press.Canvas, Support.Find("Board"), "Press is on Board")
		local label = Ui.GetRect(Support.Find("HudLabel"))
		assert(label ~= nil, "HudLabel is laid out")
		Test.ExpectEqual(label.Max, vector.create(1256, 72, 0), "the HUD label's bottom-right corner in scene pixels")
		Test.ExpectEqual(label.Canvas, Support.Find("Hud"), "HudLabel is on Hud")
	end)

	Test.Case("UiCanvas's World fields read and write through the proxy", function()
		local hud = Support.Find("Hud").UiCanvas :: UiCanvas
		Test.ExpectEqual(hud.RenderMode, "Screen", "Hud is a Screen canvas")
		local board = Support.Find("Board").UiCanvas :: UiCanvas
		Test.ExpectEqual(Support.RoundTrip(board, "WorldWidth", 1024), 800, "the scene's WorldWidth")
		Test.ExpectEqual(Support.RoundTrip(board, "WorldHeight", 576), 480, "the scene's WorldHeight")
		Test.ExpectNear(Support.RoundTrip(board, "PixelsPerUnit", 512, EPSILON), 200, EPSILON)
		Test.ExpectEqual(Support.RoundTrip(board, "RenderMode", "Screen"), "World", "the scene's Board is a World canvas")
		Support.RoundTrip(board, "RenderMode", "World")
	end)
end)
