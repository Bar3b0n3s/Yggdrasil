--!strict
-- The Light2D proxy and SpriteRenderer's NormalMap (Docs/Decisions/0032-m21-contract.md decisions 20, 21 and 32): every
-- Light2D field reads and writes through the proxy, values outside a field's range are refused, and a sprite's normal map
-- is a texture reference that reads and writes like its Texture.
local Support = require("./Support")

return Test.Suite("Api.Light2D", function()
	Test.Case("every field reads and writes through the proxy", function()
		local light = Support.Find("Lamp").Light2D :: Light2D
		Test.ExpectEqual(Support.RoundTrip(light, "Type", "Global"), "Point")
		Test.ExpectEqual(Support.RoundTrip(light, "Type", "Point"), "Global")
		local color = Color.New(0.5, 0.75, 1)
		local previous = light.Color
		light.Color = color
		Support.ExpectColorNear(light.Color, color, 1e-6, "Color reads back what was written")
		Support.ExpectColorNear(previous, Color.New(1, 0.6, 0.3), 1e-6, "the authored colour")
		Test.ExpectNear(Support.RoundTrip(light, "Intensity", 40, 1e-6), 2.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(light, "Radius", 12, 1e-6), 4, 1e-6)
		Test.ExpectNear(Support.RoundTrip(light, "Falloff", 1, 1e-6), 0.25, 1e-6)
		Test.ExpectNear(Support.RoundTrip(light, "Height", 3, 1e-6), 0.5, 1e-6)
		Test.Expect(not pcall(function()
			light.Radius = 0
		end), "a zero radius is refused")
		Test.Expect(not pcall(function()
			light.Falloff = 1.5
		end), "a falloff above 1 is refused")
		Test.Expect(not pcall(function()
			light.Intensity = -1
		end), "a negative intensity is refused")
		Test.Expect(not pcall(function()
			(light :: any).Type = "Spot"
		end), "a type Light2DType lacks is refused")
		Test.ExpectNear(light.Radius, 12, 1e-6, "a refused write leaves the field")
	end)

	Test.Case("a sprite's NormalMap reads and writes through the SpriteRenderer proxy", function()
		local normal = Assets.Load("Assets/Textures/Lights2D/Normal.png")
		local plain = Support.Find("Plain").SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(Support.RoundTrip(plain, "NormalMap", normal), nil)
		local embossed = Support.Find("Embossed").SpriteRenderer :: SpriteRenderer
		Test.ExpectEqual(Support.RoundTrip(embossed, "NormalMap", nil), normal)
		Test.ExpectEqual(embossed.Lit, true, "clearing the normal map keeps the sprite lit")
	end)
end)
