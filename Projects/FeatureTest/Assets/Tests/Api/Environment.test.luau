--!strict
-- The Environment proxy: every field read and written. Scripting.scene lights with the fallback colour (no environment
-- map), so it needs no GPU bake.
local Support = require("./Support")

return Test.Suite("Api.Environment", function()
	Test.Case("every field reads and writes through the proxy", function()
		local environment = Support.Find("Atmosphere").Environment :: Environment
		Test.ExpectEqual(Support.RoundTrip(environment, "Environment", nil), nil, "no environment map")
		Test.ExpectNear(Support.RoundTrip(environment, "Intensity", 0.5, 1e-6), 1, 1e-6)
		Support.RoundTrip(environment, "Rotation", 90, 1e-5)
		Test.ExpectEqual(Support.RoundTrip(environment, "ShowSkybox", false), true)
		Support.RoundTrip(environment, "SkyboxBlur", 0.25, 1e-6)
		local color = Color.New(0.3, 0.35, 0.4)
		local before = environment.FallbackColor
		environment.FallbackColor = color
		Support.ExpectColorNear(environment.FallbackColor, color, 1e-6, "FallbackColor reads back what was written")
		Support.ExpectColorNear(before, Color.New(0.1, 0.12, 0.2), 1e-6, "the scene's fallback colour")
		Test.Expect(not pcall(function()
			environment.SkyboxBlur = 2
		end), "a blur above one is refused")
	end)
end)
