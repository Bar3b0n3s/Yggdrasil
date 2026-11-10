--!strict
-- Sprites2D.scene (Docs/Decisions/0032-m21-contract.md decisions 3 to 7 and 17): the authored sprites, animators and
-- pixel-perfect camera keep their values in every run mode, the sheet's clips play by ticks in the SpriteAnimation phase,
-- and the built-in sprite textures load as engine textures.
local Support = require("../Api/Support")

return Test.Suite("Sprites2D", function()
	Test.Case("the authored sprites keep their values", function()
		local strider = Support.Find("Strider")
		local sprite = strider.SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(sprite.Sheet, Assets.Load("Assets/Sprites/Sprites2D/Walker.spritesheet"))
		Test.ExpectEqual(sprite.Texture, nil)
		Support.ExpectColorNear(sprite.Color, Color.New(1, 0.85, 0.7, 1), 1e-6, "Strider's tint")
		Test.ExpectEqual(sprite.PixelsPerUnit, 8)
		Test.ExpectEqual(sprite.SortingLayer, "Characters")
		Test.ExpectEqual(sprite.OrderInLayer, 2)
		Test.ExpectEqual(sprite.Lit, true)
		local animator = strider.SpriteAnimator :: SpriteAnimator
		Test.ExpectEqual(animator.Clip, "Idle")
		Test.ExpectEqual(animator.Playing, true)
		local waver = Support.Find("Waver")
		local waverSprite = waver.SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(waverSprite.FlipX, true)
		Test.ExpectEqual(waverSprite.FlipY, true)
		Test.ExpectEqual(waverSprite.Frame, 3)
		Test.ExpectEqual(waverSprite.SortingLayer, "Foreground")
		Test.ExpectEqual(waverSprite.OrderInLayer, -3)
		local waverAnimator = waver.SpriteAnimator :: SpriteAnimator
		Test.ExpectEqual(waverAnimator.Clip, "Wave")
		Test.ExpectEqual(waverAnimator.Playing, false)
		Test.ExpectEqual(waverAnimator.Step, 1)
		Test.ExpectEqual(waverAnimator.StepTicks, 2)
		local badge = Support.Find("Badge").SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(badge.Sheet, nil)
		Test.ExpectEqual(badge.Texture, Assets.Load("Assets/Textures/Sprites2D/Rgba.png"))
		Test.ExpectEqual(badge.SortingLayer, "Background")
		local camera = Support.Find("Camera").PixelPerfectCamera :: PixelPerfectCamera
		Test.ExpectEqual(camera.ReferenceWidth, 240)
		Test.ExpectEqual(camera.ReferenceHeight, 136)
		Test.ExpectEqual(camera.PixelsPerUnit, 8)
		Support.ExpectColorNear(camera.LetterboxColor, Color.New(0.1, 0.05, 0.2), 1e-6, "the letterbox colour")
	end)

	Test.Case("the Idle clip shows each step for its ticks, then loops", function()
		local strider = Support.Find("Strider")
		local sprite = strider.SpriteRenderer :: SpriteRenderer
		local animator = strider.SpriteAnimator :: SpriteAnimator
		-- Idle shows frame 0 for 8 ticks, then frame 1 for 8, and loops; the scene's Frame 1 is replaced at the first tick.
		animator:Play("Idle")
		local shown: { number } = {}
		for _ = 1, 18 do
			Test.WaitTicks(1)
			table.insert(shown, sprite.Frame)
		end
		Test.ExpectEqual(table.concat(shown, ","), "0,0,0,0,0,0,0,0,1,1,1,1,1,1,1,1,0,0")
		Test.ExpectEqual(animator.Playing, true, "a looping clip keeps playing")
	end)

	Test.Case("the built-in sprite textures load as engine textures", function()
		for _, name in { "SpriteSquare", "SpriteRounded", "SpriteBevel", "SpriteCircle" } do
			local texture = Assets.Load("engine://Textures/" .. name)
			Test.Expect(texture ~= nil, name .. " loads")
			if texture then
				Test.ExpectEqual(Assets.GetType(texture), "Texture")
			end
		end
		local shape = Support.Find("Circle").SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(shape.Texture, Assets.Load("engine://Textures/SpriteCircle"))
	end)
end)
