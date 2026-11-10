--!strict
-- The TilemapRenderer proxy (Docs/Decisions/0032-m21-contract.md decisions 24 and 32): every field reads and writes
-- through the proxy, and values outside a field's range are refused.
local Support = require("./Support")

return Test.Suite("Api.TilemapRenderer", function()
	Test.Case("every field reads and writes through the proxy", function()
		local renderer = Support.Find("Level").TilemapRenderer :: TilemapRenderer
		local level = Assets.Load("Assets/Tilemaps/Level.tilemap")
		local goal = Assets.Load("Assets/Tilemaps/Goal.tilemap")
		Test.ExpectEqual(Support.RoundTrip(renderer, "Tilemap", goal), level)
		Test.ExpectEqual(Support.RoundTrip(renderer, "Tilemap", level), goal)
		local color = Color.New(0.25, 0.5, 0.75, 0.5)
		local previous = renderer.Color
		renderer.Color = color
		Support.ExpectColorNear(renderer.Color, color, 1e-6, "Color reads back what was written")
		Support.ExpectColorNear(previous, Color.New(0.95, 0.9, 0.8, 1), 1e-6, "the authored tint")
		Test.ExpectEqual(Support.RoundTrip(renderer, "PixelsPerUnit", 16, 1e-6), 8)
		Test.ExpectEqual(Support.RoundTrip(renderer, "SortingLayer", "Foreground"), "Background")
		Test.ExpectEqual(Support.RoundTrip(renderer, "OrderInLayer", 7), -5)
		Test.ExpectEqual(Support.RoundTrip(renderer, "Lit", false), true)
		Test.Expect(not pcall(function()
			renderer.PixelsPerUnit = 0
		end), "zero pixels per unit is refused")
		Test.ExpectEqual(renderer.PixelsPerUnit, 16, "a refused write leaves the field")
	end)
end)
