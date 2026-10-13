--!strict
-- The PointLight proxy: every field read and written.
local Support = require("./Support")

return Test.Suite("Api.PointLight", function()
	Test.Case("every field reads and writes through the proxy", function()
		local light = Support.Find("Lamp").PointLight :: PointLight
		local color = Color.New(1, 0.6, 0.2)
		local before = light.Color
		light.Color = color
		Support.ExpectColorNear(light.Color, color, 1e-6, "Color reads back what was written")
		Support.ExpectColorNear(before, Color.New(1, 0.8, 0.6), 1e-6, "the scene's colour")
		Test.ExpectNear(Support.RoundTrip(light, "Intensity", 20, 1e-5), 8, 1e-5, "the scene's intensity")
		Test.ExpectNear(Support.RoundTrip(light, "Range", 4, 1e-6), 6, 1e-6, "the scene's range")
		Support.RoundTrip(light, "SourceRadius", 0.2, 1e-6)
		Test.Expect(not pcall(function()
			light.Range = 0
		end), "a zero range is refused")
	end)
end)
