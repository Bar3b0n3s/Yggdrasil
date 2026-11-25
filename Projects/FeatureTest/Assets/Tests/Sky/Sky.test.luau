--!strict
-- Sky.scene (M45; Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Sky.jsonl, IdSeed 4201): the Sky entity's Sky
-- holds every field off its default, its Sun and Moon two DirectionalLights, with no Environment beside it. Every run mode
-- reads every field through the entity.Sky shortcut's proxy, writes each, and turns the sun from a task; where the game view
-- renders (the exported Release and Dist runs, headless), the readout's SkyUpdates is 1 while the sun turns and 0 once the
-- sky stands still. The Sun's SunTurner turns it only while Fire is held (Sky.replay), so here it stands still.
local Support = require("../Api/Support")

local function Sky(): Sky
	return Support.Find("Sky").Sky :: Sky
end

-- Waits until the game view has rendered the latest state, and returns the readout.
local function RenderedStats(): PerformanceStats
	Test.WaitTicks(4)
	return Application.GetStats()
end

return Test.Suite("Sky", function()
	Test.Case("the authored sky reads back, its Sun and Moon the scene's two lights", function()
		local sky = Sky()
		Test.ExpectEqual(sky.Sun, Support.Find("Sun"), "the Sun names the sun's light")
		Test.ExpectEqual(sky.Moon, Support.Find("Moon"), "the Moon names the moon's light")
		Test.ExpectNear(sky.Intensity, 1.25, 1e-6)
		Test.ExpectEqual(sky.DriveLights, false, "DriveLights is off")
		Test.ExpectNear(sky.Haze, 1.5, 1e-6)
		Test.ExpectNear(sky.Overcast, 0.2, 1e-6)
		Support.ExpectColorNear(sky.GroundAlbedo, Color.New(0.25, 0.3, 0.2), 1e-6, "the authored ground")
		Test.ExpectNear(sky.SunDisc, 2, 1e-6)
		Test.ExpectNear(sky.MoonSize, 1.5, 1e-6)
		Test.ExpectNear(sky.Stars, 3, 1e-6)
		Test.ExpectNear(sky.Latitude, 60, 1e-6)
		Test.ExpectNear(sky.NorthAngle, 30, 1e-6)
		Test.ExpectNear(sky.Clouds, 0.4, 1e-6)
		Test.ExpectNear(sky.CloudSpeed, vector.create(3, -2, 0), 1e-6)
		Test.Screenshot("Sky")
	end)

	Test.Case("every field reads and writes through the proxy, and out-of-range values are refused", function()
		local sky = Sky()
		Support.RoundTrip(sky, "Sun", Support.Find("Sun"))
		Support.RoundTrip(sky, "Moon", nil)
		Support.RoundTrip(sky, "Moon", Support.Find("Moon"))
		Support.RoundTrip(sky, "Intensity", 2, 1e-6)
		Support.RoundTrip(sky, "DriveLights", true)
		Support.RoundTrip(sky, "Haze", 0.5, 1e-6)
		Support.RoundTrip(sky, "Overcast", 0, 1e-6)
		local ground = Color.New(0.3, 0.3, 0.3)
		sky.GroundAlbedo = ground
		Support.ExpectColorNear(sky.GroundAlbedo, ground, 1e-6, "GroundAlbedo reads back what was written")
		Support.RoundTrip(sky, "SunDisc", 1, 1e-6)
		Support.RoundTrip(sky, "MoonSize", 0.52, 1e-6)
		Support.RoundTrip(sky, "Stars", 1, 1e-6)
		Support.RoundTrip(sky, "Latitude", -30, 1e-6)
		Support.RoundTrip(sky, "NorthAngle", -90, 1e-6)
		Support.RoundTrip(sky, "Clouds", 0, 1e-6)
		Support.RoundTrip(sky, "CloudSpeed", vector.create(5, 0, 0), 1e-6)
		Test.Expect(not pcall(function()
			sky.Haze = 11
		end), "a Haze above 10 is refused")
		Test.ExpectNear(sky.Haze, 0.5, 1e-6)
		Test.Expect(not pcall(function()
			sky.Latitude = 91
		end), "a Latitude above 90 is refused")
		Test.Expect(not pcall(function()
			sky.CloudSpeed = vector.create(101, 0, 0)
		end), "a cloud speed above 100 m/s is refused")
		Test.ExpectNear(sky.CloudSpeed, vector.create(5, 0, 0), 1e-6)
	end)

	Test.Case("a task turns the sun, and the sky's light is regenerated while it turns and not once it stands still", function()
		local sky = Sky()
		-- Still clouds, so only the sun moves the sky's light.
		sky.CloudSpeed = vector.create(0, 0, 0)
		local sun = Support.Find("Sun")
		local turned = 0
		Task.Spawn(function()
			for _ = 1, 24 do
				sun.Transform:Rotate(vector.create(-1.5, 0, 0))
				turned += 1
				Task.WaitTicks(1)
			end
		end)
		local turning = RenderedStats()
		if turning.RenderAvailable then
			Test.ExpectEqual(turning.SkyUpdates, 1, "the sky's light is regenerated while the sun turns")
		else
			Test.ExpectEqual(turning.SkyUpdates, 0, "nothing renders without a renderer")
		end
		Test.WaitTicks(24)
		Test.ExpectEqual(turned, 24, "the task turned the sun every tick")
		local still = RenderedStats()
		Test.ExpectEqual(still.SkyUpdates, 0, "a still sky does not regenerate its light")
		Test.Screenshot("SkyTurned")
	end)
end)
