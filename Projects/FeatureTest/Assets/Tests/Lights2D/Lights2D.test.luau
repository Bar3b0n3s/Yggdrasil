--!strict
-- Lights2D.scene (Docs/Decisions/0032-m21-contract.md decisions 20, 21 and 32): the authored 2D lights of both types and
-- the lit, normal-mapped and unlit sprites keep their values in every run mode. A Light2D is component data and nothing
-- else (decision 29): no phase moves or changes it while the scene plays.
local Support = require("../Api/Support")

-- Expects the lights and sprites of Lights2D.scene as authored.
local function expectAuthored()
	local lamp = Support.Find("Lamp").Light2D :: Light2D
	Test.ExpectEqual(lamp.Type, "Point")
	Support.ExpectColorNear(lamp.Color, Color.New(1, 0.6, 0.3), 1e-6, "the lamp's colour")
	Test.ExpectNear(lamp.Intensity, 2.5, 1e-6)
	Test.ExpectNear(lamp.Radius, 4, 1e-6)
	Test.ExpectNear(lamp.Falloff, 0.25, 1e-6)
	Test.ExpectNear(lamp.Height, 0.5, 1e-6)
	local sky = Support.Find("Sky").Light2D :: Light2D
	Test.ExpectEqual(sky.Type, "Global")
	Support.ExpectColorNear(sky.Color, Color.New(0.2, 0.25, 0.4), 1e-6, "the sky's colour")
	Test.ExpectNear(sky.Intensity, 0.75, 1e-6)
	local embossed = Support.Find("Embossed").SpriteRenderer :: SpriteRenderer
	Test.ExpectEqual(embossed.Lit, true)
	Test.ExpectEqual(embossed.Texture, Assets.Load("Assets/Textures/Lights2D/Rgba.png"))
	Test.ExpectEqual(embossed.NormalMap, Assets.Load("Assets/Textures/Lights2D/Normal.png"))
	local plain = Support.Find("Plain").SpriteRenderer :: SpriteRenderer
	Test.ExpectEqual(plain.Lit, true)
	Test.ExpectEqual(plain.NormalMap, nil, "a lit sprite without a normal map takes the flat normal")
	local shade = Support.Find("Shade").SpriteRenderer :: SpriteRenderer
	Test.ExpectEqual(shade.Lit, false)
	Test.ExpectEqual(shade.NormalMap, nil)
end

return Test.Suite("Lights2D", function()
	Test.Case("the authored lights and sprites keep their values", function()
		expectAuthored()
	end)

	Test.Case("playing the scene leaves its lights as authored", function()
		local lamp = Support.Find("Lamp")
		local position = lamp.Transform.WorldPosition
		Test.WaitTicks(32)
		expectAuthored()
		Test.ExpectEqual(lamp.Transform.WorldPosition, position, "a light has no motion of its own")
	end)
end)
