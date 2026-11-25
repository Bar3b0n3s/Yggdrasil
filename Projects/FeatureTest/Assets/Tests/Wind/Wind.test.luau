--!strict
-- Wind.scene (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3F and 3M; Wind.jsonl, IdSeed
-- 4302): the Weather entity's Wind holds every field off its default over a grove of LodTree.gltf's trees, whose bark the
-- import's Wind setting gave a wind block (MeshLods.jsonl) and whose leaves take Assets/Materials/Wind/Leaves.material's,
-- and over a row of reeds; a Kite behaviour hangs a body in the wind (Wind:Sample each fixed step), and Jump raises the
-- wind's Strength (WindReplay.jsonl's replay). Every run mode samples the wind at three points and times against
-- velocities committed from Scene/Wind.h's double-precision model (the same bits in every configuration), reads and writes
-- every field through the proxy, and holds the sample while the world is paused.
local Support = require("../Api/Support")

local function Weather(): Wind
	return Support.Find("Weather").Wind :: Wind
end

return Test.Suite("Wind", function()
	Test.Case("the authored wind reads back and samples to the committed velocities", function()
		local wind = Weather()
		Test.ExpectNear(wind.Direction, vector.create(0.6, 0, -0.8), 1e-6, "the authored direction")
		Test.ExpectNear(wind.Strength, 12, 1e-6)
		Test.ExpectNear(wind.GustStrength, 4.5, 1e-6)
		Test.ExpectNear(wind.GustFrequency, 0.3, 1e-6)
		Test.ExpectNear(wind.Turbulence, 0.4, 1e-6)
		-- SampleWind's velocities in double precision, rounded to the vector's floats once (Scripting/Bindings/Wind.cpp).
		Test.ExpectEqual(wind:Sample(vector.create(3, 0, 4), 1.75), vector.create(10.550168991088867, 0, -12.36289119720459))
		Test.ExpectEqual(wind:Sample(vector.create(7.5, 1.25, -20), 3.5), vector.create(10.735289573669434, 0, -12.336977005004883))
		Test.ExpectEqual(wind:Sample(vector.create(-140, 0, 62.5), 900.25), vector.create(8.670680046081543, 0, -13.936844825744629))
		Test.Screenshot("Wind")
	end)

	Test.Case("every field reads and writes through the proxy as a storm comes in, and out-of-range values are refused", function()
		local wind = Weather()
		wind.Direction = vector.create(0, 0, 1)
		Test.ExpectNear(wind.Direction, vector.create(0, 0, 1), 1e-6, "Direction reads back what was written")
		Support.RoundTrip(wind, "Strength", 22, 1e-6)
		Support.RoundTrip(wind, "GustStrength", 9, 1e-6)
		Support.RoundTrip(wind, "GustFrequency", 0.6, 1e-6)
		Support.RoundTrip(wind, "Turbulence", 0.8, 1e-6)
		-- Without gusts the wind blows at its Strength along its direction everywhere and at every time.
		wind.GustStrength = 0
		Test.ExpectEqual(wind:Sample(vector.create(5, 0, -9), 3), vector.create(0, 0, 22))
		Test.ExpectEqual(wind:Sample(vector.create(-40, 2, 17), 61.5), vector.create(0, 0, 22))
		Test.Expect(not pcall(function()
			wind.Direction = vector.create(0, 1, 0)
		end), "a Direction without a horizontal part is refused")
		Test.ExpectNear(wind.Direction, vector.create(0, 0, 1), 1e-6, "a refused Direction keeps the field")
		Test.Expect(not pcall(function()
			wind.Strength = -1
		end), "a negative Strength is refused")
		Test.Expect(not pcall(function()
			wind.Turbulence = 2
		end), "a Turbulence above 1 is refused")
		Test.ExpectNear(wind.Turbulence, 0.8, 1e-6, "a refused Turbulence keeps the field")
		Test.Screenshot("WindStorm")
		-- The authored wind again, for the cases after this one.
		wind.Direction = vector.create(0.6, 0, -0.8)
		wind.Strength = 12
		wind.GustStrength = 4.5
		wind.GustFrequency = 0.3
		wind.Turbulence = 0.4
	end)

	Test.Case("Sample reads the world's clock by default, which a world pause holds", function()
		local wind = Weather()
		local probe = vector.create(-6, 1, 2.5)
		Test.WaitTicks(2)
		Test.ExpectEqual(wind:Sample(probe), wind:Sample(probe, Time.GetTime()), "the default time is the world's clock")
		Time.PauseWorld()
		-- The pause takes effect at the next fixed step.
		Test.WaitTicks(1)
		local held = Time.GetTime()
		local sample = wind:Sample(probe)
		Test.WaitTicks(30)
		Test.ExpectEqual(Time.GetTime(), held, "the world's clock holds")
		Test.ExpectEqual(wind:Sample(probe), sample, "and so does the sample")
		Time.ResumeWorld()
		Test.WaitTicks(30)
		Test.Expect(Time.GetTime() > held, "the clock moves on after the resume")
		Test.ExpectEqual(wind:Sample(probe), wind:Sample(probe, Time.GetTime()), "and the sample follows it")
		Test.Expect(wind:Sample(probe) ~= sample, "the gusts moved on")
	end)
end)
