--!strict
-- PostProcess version 2 in every run mode (M26; Docs/Decisions/0037-m26-contract.md decisions 12, 15, 17, 18 and 25;
-- PostProcessV2.jsonl, IdSeed 2405). PostProcessV2.scene's Rendering entity holds a PostProcess with every field version 2
-- added off its default: Auto exposure with its compensation, range and speeds, the Warm.cube LUT (imported from
-- Scaffold/Sources/Luts) at three quarters, SSR with its four settings, and TAA. The suite reads the authored values and
-- renders them together (the capture warms the temporal effects up from a reset history), then writes every version 2
-- field through the proxy, the built-in identity LUT and each exposure and anti-aliasing mode among them, and checks that the
-- typed Lut field refuses a built-in of another type (W26.2's review: both asset managers know the built-ins' types).
local Support = require("../Api/Support")

local WARM = "Assets/Luts/PostProcessV2/Warm.cube"
local IDENTITY = "engine://Luts/Identity"
local EXPOSURE_MODES: { ExposureMode } = { "Manual", "Auto" }
local ANTI_ALIASING: { AntiAliasing } = { "None", "Fxaa", "Taa" }

local function Post(): PostProcess
	return Support.Find("Rendering").PostProcess :: PostProcess
end

return Test.Suite("PostProcessV2", function()
	Test.Case("the authored version 2 fields read back and render together", function()
		local post = Post()
		Test.ExpectEqual(post.ExposureMode, "Auto")
		Test.ExpectNear(post.ExposureEV, 0.5, 1e-6)
		Test.ExpectNear(post.AutoExposureMinEV, -4, 1e-6)
		Test.ExpectNear(post.AutoExposureMaxEV, 8, 1e-6)
		Test.ExpectNear(post.AutoExposureSpeedUp, 2, 1e-6)
		Test.ExpectNear(post.AutoExposureSpeedDown, 0.5, 1e-6)
		local warm = Assets.Load(WARM) :: AssetRef
		Test.ExpectEqual(Assets.GetType(warm), "Lut")
		Test.ExpectEqual(post.Lut, warm, "the scene grades with the imported .cube LUT")
		Test.ExpectNear(post.LutIntensity, 0.75, 1e-6)
		Test.ExpectEqual(post.SsrEnabled, true)
		Test.ExpectNear(post.SsrIntensity, 0.8, 1e-6)
		Test.ExpectNear(post.SsrMaxDistance, 30, 1e-6)
		Test.ExpectNear(post.SsrThickness, 0.3, 1e-6)
		Test.ExpectNear(post.SsrMaxRoughness, 0.5, 1e-6)
		Test.ExpectEqual(post.AntiAliasing, "Taa")
		Test.Screenshot("PostProcessV2")
	end)

	Test.Case("every version 2 field reads and writes through the proxy", function()
		local post = Post()
		for _, mode in EXPOSURE_MODES do
			Support.RoundTrip(post, "ExposureMode", mode)
		end
		Support.RoundTrip(post, "AutoExposureMinEV", -5, 1e-6)
		Support.RoundTrip(post, "AutoExposureMaxEV", 10, 1e-6)
		Support.RoundTrip(post, "AutoExposureSpeedUp", 4, 1e-6)
		Support.RoundTrip(post, "AutoExposureSpeedDown", 1.5, 1e-6)
		local identity = Assets.Load(IDENTITY) :: AssetRef
		Test.ExpectEqual(Assets.GetType(identity), "Lut")
		Support.RoundTrip(post, "Lut", identity)
		Support.RoundTrip(post, "Lut", nil)
		Support.RoundTrip(post, "Lut", Assets.Load(WARM))
		Support.RoundTrip(post, "LutIntensity", 0.25, 1e-6)
		Support.RoundTrip(post, "SsrEnabled", false)
		Support.RoundTrip(post, "SsrIntensity", 0.5, 1e-6)
		Support.RoundTrip(post, "SsrMaxDistance", 12, 1e-6)
		Support.RoundTrip(post, "SsrThickness", 0.05, 1e-6)
		Support.RoundTrip(post, "SsrMaxRoughness", 0.25, 1e-6)
		for _, mode in ANTI_ALIASING do
			Support.RoundTrip(post, "AntiAliasing", mode)
		end
		Test.Expect(not pcall(function()
			post.SsrMaxDistance = 0
		end), "a reflected ray shorter than 0.1 m is refused")
	end)

	Test.Case("the typed Lut field refuses a built-in of another type and keeps its value", function()
		local post = Post()
		local warm = Assets.Load(WARM) :: AssetRef
		post.Lut = warm
		local white = Assets.Load("engine://Textures/White") :: AssetRef
		Test.ExpectEqual(Assets.GetType(white), "Texture")
		local ok, message = pcall(function()
			post.Lut = white
		end)
		Test.Expect(not ok, "a Texture is refused where a Lut is taken")
		Test.Expect(string.find(tostring(message), "PostProcess.Lut", 1, true) ~= nil, "the refusal names the field")
		Test.ExpectEqual(post.Lut, warm)
	end)
end)
