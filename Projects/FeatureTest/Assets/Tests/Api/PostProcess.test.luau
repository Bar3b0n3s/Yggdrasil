--!strict
-- The PostProcess proxy: every field read and written, every tone mapper and SSAO quality included.
local Support = require("./Support")

local TONEMAPPERS: { Tonemapper } = { "ACES", "PbrNeutral", "Linear", "AgX" }
local QUALITIES: { SsaoQuality } = { "Low", "High", "Medium" }

return Test.Suite("Api.PostProcess", function()
	Test.Case("every field reads and writes through the proxy", function()
		local post = Support.Find("Atmosphere").PostProcess :: PostProcess
		Support.RoundTrip(post, "ExposureEV", -1.5, 1e-6)
		for _, tonemapper in TONEMAPPERS do
			Support.RoundTrip(post, "Tonemap", tonemapper)
		end
		Test.ExpectEqual(Support.RoundTrip(post, "SsaoEnabled", true), false, "the scene turns SSAO off")
		Support.RoundTrip(post, "SsaoRadius", 0.75, 1e-6)
		Support.RoundTrip(post, "SsaoIntensity", 1.5, 1e-6)
		for _, quality in QUALITIES do
			Support.RoundTrip(post, "SsaoQuality", quality)
		end
		Support.RoundTrip(post, "BloomEnabled", false)
		Support.RoundTrip(post, "BloomIntensity", 0.1, 1e-6)
		Support.RoundTrip(post, "FxaaEnabled", false)
		Test.Expect(not pcall(function()
			post.ExposureEV = 20
		end), "an exposure beyond 16 EV is refused")
		Test.Expect(not pcall(function()
			(post :: any).Tonemap = "Filmic"
		end), "an unknown tone mapper is refused")
	end)
end)
