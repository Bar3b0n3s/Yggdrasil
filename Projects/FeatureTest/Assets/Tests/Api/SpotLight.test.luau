--!strict
-- The SpotLight proxy: every field read and written.
local Support = require("./Support")

return Test.Suite("Api.SpotLight", function()
	Test.Case("every field reads and writes through the proxy", function()
		local light = Support.Find("Spot").SpotLight :: SpotLight
		local color = Color.New(0.6, 0.8, 1)
		light.Color = color
		Support.ExpectColorNear(light.Color, color, 1e-6, "Color reads back what was written")
		Support.RoundTrip(light, "Intensity", 30, 1e-5)
		Support.RoundTrip(light, "Range", 9, 1e-6)
		Test.ExpectNear(Support.RoundTrip(light, "InnerConeAngle", 10, 1e-5), 15, 1e-5, "the scene's inner cone")
		Test.ExpectNear(Support.RoundTrip(light, "OuterConeAngle", 25, 1e-5), 35, 1e-5, "the scene's outer cone")
		Test.ExpectEqual(Support.RoundTrip(light, "CastShadows", true), false)
		Support.RoundTrip(light, "SourceRadius", 0.1, 1e-6)
		Test.Expect(not pcall(function()
			light.OuterConeAngle = 120
		end), "a cone wider than 89 degrees is refused")
	end)
end)
