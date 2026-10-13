--!strict
-- The DirectionalLight proxy: every field read and written.
local Support = require("./Support")

return Test.Suite("Api.DirectionalLight", function()
	Test.Case("every field reads and writes through the proxy", function()
		local light = Support.Find("Sun").DirectionalLight :: DirectionalLight
		local color = Color.New(1, 0.9, 0.7)
		local white = light.Color
		light.Color = color
		Support.ExpectColorNear(light.Color, color, 1e-6, "Color reads back what was written")
		Support.ExpectColorNear(white, Color.New(1, 1, 1), 0, "the default colour")
		Test.ExpectNear(Support.RoundTrip(light, "Intensity", 5, 1e-6), 3, 1e-6, "the scene's intensity")
		Test.ExpectEqual(Support.RoundTrip(light, "CastShadows", true), false, "the scene's sun casts no shadow")
		Support.RoundTrip(light, "ShadowDistance", 60, 1e-5)
		Test.ExpectEqual(Support.RoundTrip(light, "CascadeCount", 2), 4)
		Support.RoundTrip(light, "CascadeSplitLambda", 0.5, 1e-6)
		Support.RoundTrip(light, "LightAngle", 2.5, 1e-6)
		Support.RoundTrip(light, "DepthBias", 1.5, 1e-6)
		Support.RoundTrip(light, "NormalBias", 0.5, 1e-6)
		Test.Expect(not pcall(function()
			light.CascadeCount = 5
		end), "more than four cascades are refused")
	end)
end)
