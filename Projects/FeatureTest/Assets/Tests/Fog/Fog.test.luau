--!strict
-- Fog.scene (M32; Docs/Decisions/0047-m32-fog-ssr.md decisions 2, 7 and 9; Fog.jsonl, IdSeed 2901): the Haze entity's Fog
-- holds every field off its default, which every run mode reads back, renders (distance and height fog with an ambient
-- tint and a sun glow over the pillars, the skybox left clear), and reads and writes through the proxy, the way a game
-- thickens its fog as the player nears a realm's heart. Out-of-range values are refused and leave the field as it was.
local Support = require("../Api/Support")

local function Haze(): Fog
	return Support.Find("Haze").Fog :: Fog
end

return Test.Suite("Fog", function()
	Test.Case("the authored fog reads back and renders", function()
		local fog = Haze()
		Support.ExpectColorNear(fog.Color, Color.New(0.4, 0.45, 0.55), 1e-6, "the authored colour")
		Test.ExpectNear(fog.AmbientTint, 0.25, 1e-6)
		Test.ExpectNear(fog.Density, 0.05, 1e-6)
		Test.ExpectNear(fog.StartDistance, 2, 1e-6)
		Test.ExpectNear(fog.MaxOpacity, 0.9, 1e-6)
		Test.ExpectNear(fog.HeightDensity, 0.3, 1e-6)
		Test.ExpectNear(fog.HeightFalloff, 0.8, 1e-6)
		Test.ExpectNear(fog.BaseHeight, 0.5, 1e-6)
		Test.ExpectNear(fog.SunGlow, 0.6, 1e-6)
		Test.ExpectNear(fog.SunGlowExponent, 12, 1e-6)
		Test.ExpectEqual(fog.FogSkybox, false)
		Test.Screenshot("Fog")
	end)

	Test.Case("every field reads and writes through the proxy, as a game animates its fog", function()
		local fog = Haze()
		local color = Color.New(0.7, 0.3, 0.2)
		fog.Color = color
		Support.ExpectColorNear(fog.Color, color, 1e-6, "Color reads back what was written")
		Support.RoundTrip(fog, "AmbientTint", 1, 1e-6)
		Support.RoundTrip(fog, "StartDistance", 0, 1e-6)
		Support.RoundTrip(fog, "MaxOpacity", 0.5, 1e-6)
		Support.RoundTrip(fog, "HeightDensity", 1.5, 1e-6)
		Support.RoundTrip(fog, "HeightFalloff", 0.1, 1e-6)
		Support.RoundTrip(fog, "BaseHeight", -3, 1e-6)
		Support.RoundTrip(fog, "SunGlow", 4, 1e-6)
		Support.RoundTrip(fog, "SunGlowExponent", 64, 1e-6)
		Support.RoundTrip(fog, "FogSkybox", true)
		-- The fog thickens toward the realm's heart, a step at a time.
		for step = 1, 4 do
			Support.RoundTrip(fog, "Density", 0.05 * (step + 1), 1e-6)
		end
		Test.Screenshot("FogThickened")
	end)

	Test.Case("out-of-range values are refused and keep the field", function()
		local fog = Haze()
		local density = fog.Density
		Test.Expect(not pcall(function()
			fog.Density = -1
		end), "a negative density is refused")
		Test.ExpectNear(fog.Density, density, 1e-6)
		Test.Expect(not pcall(function()
			fog.MaxOpacity = 1.5
		end), "an opacity above one is refused")
		Test.Expect(not pcall(function()
			fog.SunGlowExponent = 0.5
		end), "a glow exponent below one is refused")
	end)
end)
