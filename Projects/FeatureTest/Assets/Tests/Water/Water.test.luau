--!strict
-- Water and buoyancy in every run mode (M47; Docs/Decisions/0065-m47-water.md decision 3; Water.jsonl, IdSeed 4401).
-- Water.scene holds a Sea (the Ocean at y = 0, three waves at WaveScale 0.8), a raised Pool (a Rectangle 6 m wide around
-- (16, 3, 0), 2.5 m deep with 3 m of headroom, a current, a denser water and its own maps), and a Grotto (a calm
-- Rectangle at y = -6 inside a closed rock box at -9 to -2, its Headroom reaching the ceiling, so the sea is masked inside
-- it); a 400 kg Boat with four box pontoons and a Buoyancy at its defaults, and a Crate whose Buoyancy names the Pool.
-- Each case starts the scene afresh (Isolation "Case").
local Support = require("../Api/Support")

-- The Boat's analytic draft on calm fresh water: rho V_sub = m gives 400 / 1000 = 0.4 m^3 under the surface, over its four
-- 1.2 m x 0.6 m pontoon footprints (2.88 m^2) a draft of 0.4 / 2.88 m. The pontoons are 0.5 m tall and centred on the
-- Boat's origin, so it rests 0.25 m - draft above the surface.
local DRAFT = 0.4 / 2.88
local REST_HEIGHT = 0.25 - DRAFT

-- The fixed steps in a second of world time (64 in FeatureTest).
local function TicksPerSecond(): number
	return math.round(1 / Time.GetFixedDeltaTime())
end

local function Water(name: string): Water
	return Support.Find(name).Water :: Water
end

-- The Boat's pitch (about its local X axis's tilt) and roll (its local Z axis's tilt), in degrees.
local function Attitude(boat: Entity): (number, number)
	local rotation = boat.Transform.WorldRotation
	local axisX = rotation * vector.create(1, 0, 0)
	local axisZ = rotation * vector.create(0, 0, 1)
	return math.deg(math.asin(axisX.y)), math.deg(math.asin(axisZ.y))
end

return Test.Suite("Water", function()
	Test.Case("the boat dropped on calm water sits at its analytic draft within 2 cm after 5 s", function()
		Water("Sea").WaveScale = 0
		local boat = Support.Find("Boat")
		boat.Transform:Teleport(vector.create(0, REST_HEIGHT + 0.5, 0), Quat.Identity())
		Test.WaitTicks(5 * TicksPerSecond())
		-- From 5 s on, for a second, it stays within 2 cm of its draft, level.
		local farthest = 0
		for _ = 1, TicksPerSecond() do
			Test.WaitTicks(1)
			farthest = math.max(farthest, math.abs(boat.Transform.WorldPosition.y - REST_HEIGHT))
		end
		Test.Expect(farthest < 0.02, `the boat stays within 2 cm of its draft after 5 s (at most {farthest} m off)`)
		local pitch, roll = Attitude(boat)
		Test.Expect(math.abs(pitch) < 0.5 and math.abs(roll) < 0.5, `the boat floats level ({pitch}, {roll} degrees)`)
	end)

	Test.Case("on WaveScale 2 the boat pitches and rolls", function()
		Water("Sea").WaveScale = 2
		local boat = Support.Find("Boat")
		Test.WaitTicks(TicksPerSecond())
		local lowPitch: number = 0
		local highPitch: number = 0
		local lowRoll: number = 0
		local highRoll: number = 0
		for _ = 1, 3 * TicksPerSecond() do
			Test.WaitTicks(1)
			local pitch, roll = Attitude(boat)
			lowPitch = math.min(lowPitch, pitch)
			highPitch = math.max(highPitch, pitch)
			lowRoll = math.min(lowRoll, roll)
			highRoll = math.max(highRoll, roll)
		end
		Test.Expect(highPitch - lowPitch > 3, `the boat pitches ({lowPitch} to {highPitch} degrees)`)
		Test.Expect(highRoll - lowRoll > 3, `the boat rolls ({lowRoll} to {highRoll} degrees)`)
		-- It floats on the swell, never sinks nor flies.
		Test.Expect(math.abs(boat.Transform.WorldPosition.y) < 1.5, "the boat stays on the surface")
	end)

	Test.Case("SampleHeight equals the base at calm and follows a tide write", function()
		local sea = Water("Sea")
		local point = vector.create(3.5, -1, -2)
		Test.Expect(sea:SampleHeight(point) ~= 0, "the swell moves the surface off its base")
		sea.WaveScale = 0
		Test.ExpectEqual(sea:SampleHeight(point), 0, "a calm sea lies at its base")
		Test.ExpectNear(sea:SampleNormal(point) :: vector, vector.create(0, 1, 0), 1e-6, "a calm sea's normal is up")
		Test.ExpectNear(sea:SampleVelocity(point) :: vector, vector.zero, 1e-6, "a calm sea without a current is still")
		Test.Expect(sea:IsUnderwater(point), "a point below the base is under water")
		-- The tide: the base rises with the entity's Translation.
		Support.Find("Sea").Transform.Translation = vector.create(0, 1.5, 0)
		Test.WaitTicks(1)
		Test.ExpectEqual(sea:SampleHeight(point), 1.5, "the surface follows the tide")
		Test.Expect(sea:IsUnderwater(vector.create(3.5, 1, -2)), "a point under the raised base is under water")
		-- A time argument reads the surface at that world time; the calm sea is the same at any time.
		Test.ExpectEqual(sea:SampleHeight(point, 12.5), 1.5, "a calm sea at any time")
		-- The Pool's own surface: its current, and nothing outside its footprint.
		local pool = Water("Pool")
		Test.ExpectNear((pool:SampleVelocity(vector.create(16, 3, 0), 0) :: vector).x, 0.8, 0.05, "the Pool's current flows along +X")
		Test.ExpectEqual(pool:SampleHeight(vector.create(40, 3, 0)), nil, "outside the Pool's footprint there is no surface")
		Test.Expect(not pool:IsUnderwater(vector.create(16, -5, 0)), "below the Pool's volume is not in it")
	end)

	Test.Case("Physics.SampleWater answers the Pool over the sea and dry air in the grotto above its lake", function()
		local pool = Physics.SampleWater(vector.create(16, 2.5, 1)) :: WaterSample
		Test.Expect(pool ~= nil and pool.Water == Support.Find("Pool"), "the raised Pool holds a point in its volume")
		Test.ExpectNear(pool.Depth, 0.5, 0.05, "half a metre under the Pool's surface")
		Test.ExpectNear(pool.Velocity.x, 0.8, 0.05, "the Pool's current")
		local air = Physics.SampleWater(vector.create(-16, -4, 0)) :: WaterSample
		Test.Expect(air ~= nil and air.Water == Support.Find("Grotto"), "the Grotto, not the sea, holds the cave's air")
		Test.ExpectNear(air.Depth, -2, 1e-6, "dry: 2 m above the calm lake, though 4 m below the sea's level")
		local lake = Physics.SampleWater(vector.create(-16, -7.5, 0)) :: WaterSample
		Test.Expect(lake ~= nil and lake.Water == Support.Find("Grotto"), "the Grotto holds its lake")
		Test.ExpectNear(lake.Depth, 1.5, 1e-6, "under the lake's surface")
		Test.ExpectNear(lake.Normal, vector.create(0, 1, 0), 1e-6, "a calm lake's normal is up")
		local sea = Physics.SampleWater(vector.create(0, -20, 30)) :: WaterSample
		Test.Expect(sea ~= nil and sea.Water == Support.Find("Sea"), "elsewhere the Ocean holds every point")
		Test.Expect(sea.Depth > 18, "deep under the sea")
		Test.Expect(not pcall(function()
			Physics.SampleWater(vector.create(0, 0, 2e9))
		end), "a point beyond the world is refused")
	end)

	Test.Case("a Buoyancy naming the Pool floats its crate on the Pool alone", function()
		local crate = Support.Find("Crate")
		Test.WaitTicks(3 * TicksPerSecond())
		local height = crate.Transform.WorldPosition.y
		-- 250 kg at BuoyancyScale 1.2 in the Pool's 1025 kg/m^3: 250 / (1.2 x 1025) m^3 of the 1 m cube under the surface,
		-- the centre 0.5 m above its bottom. The Pool's ripple is 5 mm.
		local expected = 3 + 0.5 - 250 / (1.2 * 1025)
		Test.ExpectNear(height, expected, 0.05, "the crate rests at its draft in the Pool, 3 m above the sea")
	end)

	Test.Case("every Water field reads and writes through the proxy, and out-of-range values are refused", function()
		local water = Water("Pool")
		Test.ExpectEqual(Support.RoundTrip(water, "Shape", "Ocean"), "Rectangle")
		water.Shape = "Rectangle"
		Test.ExpectEqual(Support.RoundTrip(water, "Size", vector.create(5, 7, 0)), vector.create(6, 6, 0))
		Test.ExpectEqual(Support.RoundTrip(water, "Depth", 1.5), 2.5)
		Test.ExpectEqual(Support.RoundTrip(water, "Headroom", 4), 3)
		local waves = water.Waves
		Test.ExpectEqual(#waves, 1)
		Test.ExpectEqual(waves[1].Direction, 30)
		water.Waves = {
			{ Direction = -45, Wavelength = 12, Amplitude = 0.25, Steepness = 0.75, Speed = 4, Phase = 90 },
			{ Direction = 10, Wavelength = 3, Amplitude = 0.125, Steepness = 0, Speed = 0, Phase = 0 },
		}
		local written = water.Waves
		Test.ExpectEqual(#written, 2)
		Test.ExpectEqual(written[1].Direction, -45)
		Test.ExpectEqual(written[1].Wavelength, 12)
		Test.ExpectEqual(written[1].Amplitude, 0.25)
		Test.ExpectEqual(written[1].Steepness, 0.75)
		Test.ExpectEqual(written[1].Speed, 4)
		Test.ExpectEqual(written[1].Phase, 90)
		Test.ExpectEqual(written[2].Wavelength, 3)
		Test.ExpectEqual(Support.RoundTrip(water, "WaveScale", 1.5), Support.F32(1))
		Test.ExpectEqual(Support.RoundTrip(water, "Current", vector.create(-0.5, 0.25, 0)), vector.create(Support.F32(0.8), 0, 0))
		Test.ExpectEqual(Support.RoundTrip(water, "Density", 1200), 1025)
		Support.ExpectColorNear(Support.RoundTrip(water, "ShallowColor", Color.New(0.25, 0.5, 0.75)), Color.New(0.2, 0.6, 0.55), 1e-6, "ShallowColor")
		Support.ExpectColorNear(Support.RoundTrip(water, "DeepColor", Color.New(0, 0.125, 0.25)), Color.New(0.02, 0.1, 0.12), 1e-6, "DeepColor")
		Test.ExpectEqual(Support.RoundTrip(water, "Clarity", 8), 2)
		Test.ExpectEqual(Support.RoundTrip(water, "Roughness", 0.25), Support.F32(0.1))
		Test.ExpectEqual(Support.RoundTrip(water, "RefractionStrength", 0.75), Support.F32(0.2))
		Test.ExpectEqual(Support.RoundTrip(water, "FoamAmount", 0.25), Support.F32(0.3))
		Test.ExpectEqual(Support.RoundTrip(water, "ShoreFoamDepth", 2), 0.25)
		Test.ExpectEqual(Support.RoundTrip(water, "DetailNormalStrength", 0.5), Support.F32(0.8))
		Test.ExpectEqual(Support.RoundTrip(water, "DetailScale", 6), 2)
		Test.ExpectEqual(Support.RoundTrip(water, "DetailDrift", vector.create(0.5, -1, 0)), vector.create(-1, 0.5, 0))
		Support.ExpectColorNear(Support.RoundTrip(water, "UnderwaterColor", Color.New(0.125, 0.25, 0.25)), Color.New(0.05, 0.15, 0.1), 1e-6, "UnderwaterColor")
		Test.ExpectEqual(Support.RoundTrip(water, "UnderwaterVisibility", 30), 8)
		local normalMap = water.NormalMap
		local foam = water.FoamTexture
		Test.Expect(normalMap ~= nil and foam ~= nil, "the Pool's own maps")
		Test.ExpectEqual(Support.RoundTrip(water, "NormalMap", foam), normalMap)
		Test.ExpectEqual(Support.RoundTrip(water, "FoamTexture", nil), foam)
		-- Out of range: refused, the field kept.
		Test.Expect(not pcall(function()
			water.Density = 0
		end), "a Density below 1 is refused")
		Test.ExpectEqual(water.Density, 1200, "a refused Density keeps the field")
		Test.Expect(not pcall(function()
			water.Roughness = 0
		end), "a Roughness below 0.02 is refused")
		Test.Expect(not pcall(function()
			water.Waves = { { Direction = 0, Wavelength = 0.01, Amplitude = 0.1, Steepness = 0.5, Speed = 0, Phase = 0 } }
		end), "a Wavelength below 0.1 m is refused")
		local nine = {}
		for index = 1, 9 do
			table.insert(nine, { Direction = index * 10, Wavelength = 20, Amplitude = 0.1, Steepness = 0.1, Speed = 0, Phase = 0 })
		end
		Test.Expect(not pcall(function()
			water.Waves = nine
		end), "a ninth wave is refused")
		Test.ExpectEqual(#water.Waves, 2, "a refused list keeps the waves")
	end)

	Test.Case("every Buoyancy field reads and writes through the proxy", function()
		local buoyancy = Support.Find("Crate").Buoyancy :: Buoyancy
		Test.ExpectEqual(Support.RoundTrip(buoyancy, "Water", Support.Find("Sea")), Support.Find("Pool"))
		Test.ExpectEqual(Support.RoundTrip(buoyancy, "Water", nil), Support.Find("Sea"))
		Test.ExpectEqual(Support.RoundTrip(buoyancy, "BuoyancyScale", 0.5), Support.F32(1.2))
		Test.ExpectEqual(Support.RoundTrip(buoyancy, "LinearDrag", 4), 3)
		Test.ExpectEqual(Support.RoundTrip(buoyancy, "AngularDrag", 0.5), 0.25)
		Test.Expect(not pcall(function()
			buoyancy.BuoyancyScale = 11
		end), "a BuoyancyScale above 10 is refused")
		Test.Expect(not pcall(function()
			buoyancy.LinearDrag = -1
		end), "a negative drag is refused")
	end)
end)
