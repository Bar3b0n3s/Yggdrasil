--!strict
-- The SpriteRenderer proxy (Docs/Decisions/0032-m21-contract.md decisions 3 and 13): every field read and written, and
-- SetFrameByName selects a sheet frame by its name.
local Support = require("./Support")

return Test.Suite("Api.SpriteRenderer", function()
	Test.Case("SetFrameByName sets the named frame of the sheet", function()
		local sprite = Support.Find("Waver").SpriteRenderer :: SpriteRenderer
		sprite:SetFrameByName("Step1")
		Test.ExpectEqual(sprite.Frame, 3)
		sprite:SetFrameByName("Idle0")
		Test.ExpectEqual(sprite.Frame, 0)
		Test.Expect(not pcall(function()
			sprite:SetFrameByName("Nowhere")
		end), "a name the sheet lacks is an error")
		Test.ExpectEqual(sprite.Frame, 0, "a refused name leaves the frame")
		local badge = Support.Find("Badge").SpriteRenderer :: SpriteRenderer
		Test.Expect(not pcall(function()
			badge:SetFrameByName("Idle0")
		end), "a sprite without a sheet has no named frames")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local sprite = Support.Find("Badge").SpriteRenderer :: SpriteRenderer
		local sheet = Assets.Load("Assets/Sprites/Sprites2D/Walker.spritesheet")
		local texture = Assets.Load("Assets/Textures/Sprites2D/Rgba.png")
		Test.ExpectEqual(Support.RoundTrip(sprite, "Sheet", sheet), nil)
		Test.ExpectEqual(Support.RoundTrip(sprite, "Texture", nil), texture)
		sprite.Texture = texture
		Test.ExpectEqual(Support.RoundTrip(sprite, "Frame", 2), 0)
		local color = Color.New(0.25, 0.5, 0.75, 0.5)
		local previous = sprite.Color
		sprite.Color = color
		Support.ExpectColorNear(sprite.Color, color, 1e-6, "Color reads back what was written")
		Support.ExpectColorNear(previous, Color.New(1, 1, 1, 1), 0, "the default colour")
		Test.ExpectEqual(Support.RoundTrip(sprite, "FlipX", true), false)
		Test.ExpectEqual(Support.RoundTrip(sprite, "FlipY", true), false)
		Test.ExpectEqual(Support.RoundTrip(sprite, "PixelsPerUnit", 32, 1e-6), 4)
		Test.ExpectEqual(Support.RoundTrip(sprite, "SortingLayer", "Foreground"), "Background")
		Test.ExpectEqual(Support.RoundTrip(sprite, "OrderInLayer", -7), 5)
		Test.ExpectEqual(Support.RoundTrip(sprite, "Lit", true), false)
		Test.Expect(not pcall(function()
			sprite.PixelsPerUnit = 0
		end), "zero pixels per unit is refused")
	end)
end)
