--!strict
-- Vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Skerries' S02) in Vehicles.scene (Vehicles.jsonl,
-- IdSeed 4101), in every run mode. The Buggy (Auto, Cylinder casts, four-wheel drive through two differentials, two anti-roll
-- bars and a MassProperties that lowers its centre of mass) starts on a 2 m ledge above a long flat ground; the Cart (Manual,
-- Ray casts) waits in gear with its clutch open; the Rig (Sphere casts) is parked on the Hill terrain's slope with its brakes
-- and hand brake on; a Slick strip (ColliderSurface Friction 0.3) lies beside a Rough one (1.0). The cases check S02's four
-- checks (the rest length, the analytic top speed, a full-lock turn that does not roll, a 2 m drop that lands on its wheels),
-- the slick-against-rough stop, Manual's gears, the hand brake, every read with its errors, and every field through the
-- proxies. Each case starts from a fresh load of the scene (Isolation "Case").
local Support = require("../Api/Support")

local WHEELS = { "FL", "FR", "RL", "RR" }

local function find(path: string): Entity
	local entity = Scene.FindByPath(path)
	assert(entity, `{path} is in the scene`)
	return entity
end

local function vehicle(path: string): WheeledVehicle
	local component = find(path).WheeledVehicle
	assert(component, `{path} has a WheeledVehicle`)
	return component
end

local function wheel(path: string): VehicleWheel
	local component = find(path).VehicleWheel
	assert(component, `{path} has a VehicleWheel`)
	return component
end

local function body(path: string): RigidBody
	local component = find(path).RigidBody
	assert(component, `{path} has a RigidBody`)
	return component
end

-- How upright the entity stands: the world Y of its up.
local function upright(path: string): number
	return find(path).Transform:TransformDirection(vector.create(0, 1, 0)).y
end

-- Expects a curve's keys to equal `expected`'s (each X and Y within a float's rounding).
local function expectKeys(actual: { { X: number, Y: number } }, expected: { { X: number, Y: number } }, message: string)
	Test.ExpectEqual(#actual, #expected, message)
	for index, key in expected do
		Test.ExpectNear(actual[index].X, key.X, 1e-5, message)
		Test.ExpectNear(actual[index].Y, key.Y, 1e-5, message)
	end
end

-- Expects a list of numbers to equal `expected` within a float's rounding.
local function expectNumbers(actual: { number }, expected: { number }, message: string)
	Test.ExpectEqual(#actual, #expected, message)
	for index, value in expected do
		Test.ExpectNear(actual[index], value, 1e-5, message)
	end
end

return Test.Suite("Vehicles", function()
	Test.Case("at rest the cart touches with every wheel within 1 cm of its rest length, and the buggy with all four", function()
		Test.WaitTicks(300)
		local cart = find("/Cart")
		local mass = body("/Cart").Mass
		local box = cart.BoxCollider
		assert(box, "the cart's chassis is a box")
		local size = box.HalfExtents * 2
		-- The box's inertia about its centre, which is its centre of mass: the Cart has no MassProperties.
		local inertiaX = mass / 12 * (size.y * size.y + size.z * size.z)
		local inertiaZ = mass / 12 * (size.x * size.x + size.y * size.y)
		local weight = mass * -Physics.GetGravity().y
		local centre = cart.Transform.WorldPosition
		for _, name in WHEELS do
			local tyre = wheel(`/Cart/{name}`)
			Test.Expect(tyre:IsTouching(), `the cart's {name} touches the ground`)
			local point = tyre:GetContact()
			assert(point, "a touching wheel has a contact")
			-- Each spring holds a quarter of the weight. Jolt's spring of frequency f has the stiffness m_eff (2 pi f)^2, m_eff the
			-- cart's effective mass along the suspension at the contact, 1 / (1/m + (r x n) I^-1 (r x n)) with r from the centre of
			-- mass: its ray casts put each contact straight under its wheel (a cylinder's deepest point lies off the wheel's plane).
			local lever = point - centre
			local effective = 1 / (1 / mass + lever.z * lever.z / inertiaX + lever.x * lever.x / inertiaZ)
			local omega = 2 * math.pi * tyre.SpringFrequency
			local rest = tyre.SuspensionMaxLength - weight / 4 / (effective * omega * omega)
			Test.ExpectNear(tyre:GetSuspensionLength(), rest, 0.01, `the cart's {name} rests within 1 cm of its rest length`)
			Test.ExpectNear(tyre:GetSuspensionForce(), weight / 4, weight / 4 * 0.02, `the cart's {name} carries a quarter of the weight`)
		end
		for _, name in WHEELS do
			local tyre = wheel(`/Buggy/{name}`)
			Test.Expect(tyre:IsTouching(), `the buggy's {name} touches the ledge`)
			local length = tyre:GetSuspensionLength()
			Test.Expect(length > tyre.SuspensionMinLength and length < tyre.SuspensionMaxLength, `the buggy's {name} rests on its spring`)
		end
		Test.Expect(upright("/Buggy") > 0.999, "the buggy stands level")
	end, { TimeoutTicks = 400 })

	Test.Case("at full throttle the buggy reaches its analytic top speed and holds it within 2 %", function()
		local buggy = vehicle("/Buggy")
		buggy.Throttle = 1
		-- MaxRpm 2 pi / 60 / (top ratio * differential ratio) * radius. The torque curve falls to 0 at MaxRpm (a rev limiter):
		-- Jolt keeps the engine's torque at MaxRpm, so a curve that ends above 0 would drive the wheels past it.
		local top = buggy.MaxRpm * 2 * math.pi / 60 / (buggy.GearRatios[#buggy.GearRatios] * buggy.Differentials[1].Ratio)
			* wheel("/Buggy/FL").Radius
		Test.WaitTicks(720)
		Test.ExpectEqual(buggy:GetGear(), #buggy.GearRatios, "the buggy drives in its top gear")
		for _ = 1, 8 do
			Test.ExpectNear(buggy:GetSpeed(), top, top * 0.02, "the buggy holds its top speed")
			Test.ExpectNear(buggy:GetEngineRpm(), buggy.MaxRpm, buggy.MaxRpm * 0.02, "at its MaxRpm")
			Test.WaitTicks(15)
		end
	end, { TimeoutTicks = 900 })

	Test.Case("a full-lock turn at 15 m/s does not roll the buggy over with its centre of mass lowered", function()
		local buggy = vehicle("/Buggy")
		local entity = find("/Buggy")
		entity.Transform:Teleport(vector.create(0, 0.72, -60))
		Test.WaitTicks(60)
		body("/Buggy"):SetLinearVelocity(entity.Transform:Forward() * 15)
		local heading = entity.Transform:Forward()
		buggy.Throttle = 0.4
		buggy.Steering = 1
		local lowest = 1
		for _ = 1, 120 do
			Test.WaitTicks(1)
			lowest = math.min(lowest, upright("/Buggy"))
		end
		Test.ExpectNear(wheel("/Buggy/FL"):GetSteerAngle(), wheel("/Buggy/FL").MaxSteerAngle, 1e-3, "full right lock")
		Test.Expect(vector.dot(entity.Transform:Forward(), heading) < 0.87, "the buggy turned more than 30 degrees")
		Test.Expect(lowest > 0.9, `the buggy stays upright through the turn: {lowest}`)
		local wheels = 0
		for _, name in WHEELS do
			if wheel(`/Buggy/{name}`):IsTouching() then
				wheels += 1
			end
		end
		Test.ExpectEqual(wheels, 4, "on all four wheels")
	end, { TimeoutTicks = 300 })

	Test.Case("the buggy drives off the 2 m ledge and lands on its wheels", function()
		local buggy = vehicle("/Buggy")
		local entity = find("/Buggy")
		Test.WaitTicks(30)
		local height = entity.Transform.WorldPosition.y
		Test.Expect(height > 2, "the buggy starts on the ledge")
		buggy.Throttle = 1
		Test.WaitUntil(function()
			return not wheel("/Buggy/RL"):IsTouching() and not wheel("/Buggy/FL"):IsTouching()
		end, 400)
		Test.WaitUntil(function()
			return wheel("/Buggy/FL"):IsTouching() or wheel("/Buggy/RL"):IsTouching()
		end, 120)
		Test.Expect(entity.Transform.WorldPosition.y < height - 1.5, "it landed on the ground below")
		local lowest = 1
		for _ = 1, 90 do
			Test.WaitTicks(1)
			lowest = math.min(lowest, upright("/Buggy"))
		end
		Test.Expect(lowest > 0.8, `it lands on its wheels and stays on them: {lowest}`)
		for _, name in WHEELS do
			Test.Expect(wheel(`/Buggy/{name}`):IsTouching(), `{name} is on the ground`)
		end
	end, { TimeoutTicks = 700 })

	Test.Case("the buggy stops shorter on the rough strip than on the slick one", function()
		local buggy = vehicle("/Buggy")
		local entity = find("/Buggy")
		local function stop(x: number, strip: string): number
			entity.Transform:Teleport(vector.create(x, 0.72, 0))
			Test.WaitTicks(60)
			local _, _, ground = wheel("/Buggy/RL"):GetContact()
			Test.ExpectEqual(ground, find(strip), `the buggy stands on {strip}`)
			body("/Buggy"):SetLinearVelocity(entity.Transform:Forward() * 12)
			buggy.Brake = 1
			local start = entity.Transform.WorldPosition
			Test.WaitUntil(function()
				return math.abs(buggy:GetSpeed()) < 0.1
			end, 600)
			buggy.Brake = 0
			return vector.magnitude(entity.Transform.WorldPosition - start)
		end
		local slick = stop(-66, "/Track/Slick")
		local rough = stop(-78, "/Track/Rough")
		Test.Expect(rough < slick * 0.8, `the rough strip stops it in {rough} m, the slick one in {slick} m`)
	end, { TimeoutTicks = 900 })

	Test.Case("the cart drives in Manual's gears, forward and in reverse", function()
		local cart = vehicle("/Cart")
		Test.WaitTicks(30)
		Test.ExpectEqual(cart:GetGear(), 1, "Manual holds the authored gear")
		Test.ExpectNear(cart:GetSpeed(), 0, 0.05, "the open clutch drives nothing")
		Test.Expect(cart:GetEngineRpm() > cart.MinRpm, "the engine revs against the open clutch")
		cart.Clutch = 1
		Test.WaitTicks(60)
		Test.Expect(cart:GetSpeed() > 1, "the closed clutch drives the cart forward in first")
		cart.Gear = 2
		Test.WaitTicks(2)
		Test.ExpectEqual(cart:GetGear(), 2, "second gear")
		cart.Gear = 8
		Test.WaitTicks(2)
		Test.ExpectEqual(cart:GetGear(), #cart.GearRatios, "a gear past the ratios is clamped to the top one")
		cart.Gear = 0
		cart.Throttle = 0
		cart.Brake = 1
		Test.WaitUntil(function()
			return math.abs(cart:GetSpeed()) < 0.05
		end, 300)
		Test.ExpectEqual(cart:GetGear(), 0, "neutral")
		cart.Brake = 0
		cart.Gear = -1
		cart.Throttle = 1
		Test.WaitTicks(60)
		Test.ExpectEqual(cart:GetGear(), -1, "first reverse")
		Test.Expect(cart:GetSpeed() < -0.5, "reverse drives the cart backward")
		cart.Gear = -8
		Test.WaitTicks(2)
		Test.ExpectEqual(cart:GetGear(), -#cart.ReverseGearRatios, "a reverse gear past the ratios is clamped")
		Test.Expect(not cart:IsShifting(), "Manual never shifts by itself")
	end, { TimeoutTicks = 600 })

	Test.Case("the parked rig holds on the hill's slope with its hand brake, and rolls downhill once released", function()
		local rig = vehicle("/Rig")
		Test.WaitTicks(120)
		Test.ExpectNear(rig:GetSpeed(), 0, 0.05, "the brakes hold the rig on the slope")
		for _, name in WHEELS do
			local tyre = wheel(`/Rig/{name}`)
			Test.Expect(tyre:IsTouching(), `the rig's {name} stands on the hill`)
			local _, normal, ground = tyre:GetContact()
			Test.ExpectEqual(ground, find("/Hill"), `the rig's {name} stands on the hill's collider`)
			assert(normal, "a touching wheel has a normal")
			Test.Expect(normal.x < -0.1, "the hill rises along +X")
		end
		Test.Expect(upright("/Rig") < 0.995, "the rig leans with the slope")
		local start = find("/Rig").Transform.WorldPosition
		rig.HandBrake = 0
		rig.Brake = 0
		Test.WaitTicks(120)
		Test.Expect(find("/Rig").Transform.WorldPosition.x < start.x - 0.5, "released, the rig rolls downhill")
		Test.Expect(rig:GetSpeed() > 0.5, "forward, its nose downhill")
	end, { TimeoutTicks = 400 })

	Test.Case("a vehicle reads its speed, revs and gear and each wheel its contact, suspension, spin, steer and slip; one not in force raises", function()
		local buggy = vehicle("/Buggy")
		buggy.Throttle = 1
		buggy.Steering = 0.5
		Test.WaitTicks(60)
		Test.Expect(buggy:GetSpeed() > 1, "the buggy drives forward")
		Test.Expect(buggy:GetEngineRpm() >= buggy.MinRpm and buggy:GetEngineRpm() <= buggy.MaxRpm, "revs between MinRpm and MaxRpm")
		Test.Expect(buggy:GetGear() >= 1, "Auto drives in a forward gear")
		Test.ExpectEqual(type(buggy:IsShifting()), "boolean")
		local ledge = find("/Track/Ledge")
		for _, name in WHEELS do
			local tyre = wheel(`/Buggy/{name}`)
			Test.Expect(tyre:IsTouching(), `{name} touches`)
			local point, normal, collider = tyre:GetContact()
			assert(point and normal, "a touching wheel has a contact")
			Test.ExpectNear(point.y, 2, 0.05, `{name} touches the ledge's top`)
			Test.ExpectNear(normal, vector.create(0, 1, 0), 1e-3, `{name} stands on a level ground`)
			Test.ExpectEqual(collider, ledge, `{name} stands on the ledge's collider`)
			local length = tyre:GetSuspensionLength()
			Test.Expect(length >= tyre.SuspensionMinLength - 1e-4 and length < tyre.SuspensionMaxLength, `{name}'s spring is compressed`)
			Test.Expect(tyre:GetSuspensionForce() > 0, `{name} carries part of the weight`)
			local rotation = tyre:GetRotation()
			Test.Expect(rotation >= 0 and rotation < 360, `{name}'s spin is in [0, 360)`)
			Test.Expect(tyre:GetAngularVelocity() > 0, `{name} rolls forward`)
			local ratio, angle = tyre:GetSlip()
			Test.Expect(ratio >= 0 and angle >= 0 and angle <= 90, `{name}'s slips are magnitudes`)
		end
		Test.ExpectNear(wheel("/Buggy/FL"):GetSteerAngle(), 0.5 * wheel("/Buggy/FL").MaxSteerAngle, 1e-3, "half a right lock")
		Test.ExpectEqual(wheel("/Buggy/RL"):GetSteerAngle(), 0, "the rear wheels do not steer")
		-- The spin advances by its speed: one tick's turn, in [0, 360).
		local spinning = wheel("/Buggy/RR")
		local before = spinning:GetRotation()
		local speed = spinning:GetAngularVelocity()
		Test.WaitTicks(1)
		local turned = (spinning:GetRotation() - before) % 360
		Test.ExpectNear(turned, (speed + spinning:GetAngularVelocity()) / 2 / 60, math.abs(speed) / 60 * 0.1, "the spin advances by its speed")

		-- A write that leaves the cart without a differential takes it out of force at the next physics pre-step.
		local cart = vehicle("/Cart")
		cart.Differentials = {}
		Test.ExpectScriptError("PHYSICS_VEHICLE_INVALID", 2)
		local function expectNotInForce(read: () -> (), what: string)
			local ok, message = pcall(read)
			Test.Expect(not ok and string.find(tostring(message), "entity has no vehicle in force", 1, true) ~= nil, `{what}: {tostring(message)}`)
		end
		expectNotInForce(function()
			local _ = cart:GetSpeed()
		end, "WheeledVehicle:GetSpeed")
		expectNotInForce(function()
			local _ = cart:GetEngineRpm()
		end, "WheeledVehicle:GetEngineRpm")
		expectNotInForce(function()
			local _ = wheel("/Cart/FL"):GetSuspensionLength()
		end, "VehicleWheel:GetSuspensionLength")
		expectNotInForce(function()
			local _ = wheel("/Cart/RR"):GetContact()
		end, "VehicleWheel:GetContact")
	end, { TimeoutTicks = 200 })

	Test.Case("every vehicle field reads and writes through its proxy", function()
		local mass = find("/Buggy").MassProperties
		assert(mass, "the buggy has a MassProperties")
		Test.ExpectNear(Support.RoundTrip(mass, "CenterOfMassOffset", vector.create(0, -0.5, 0.1)), vector.create(0, -0.6, 0), 1e-6)
		Test.ExpectNear(Support.RoundTrip(mass, "InertiaScale", vector.create(1.2, 0.9, 1)), vector.create(1.1, 1, 1.1), 1e-6)

		local rig = vehicle("/Rig")
		local fl, fr, rl, rr = find("/Rig/FL"), find("/Rig/FR"), find("/Rig/RL"), find("/Rig/RR")
		Test.ExpectNear(Support.RoundTrip(rig, "Throttle", 0.25, 1e-6), 0, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "Steering", 0.75, 1e-6), -0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "Brake", 0.5, 1e-6), 1, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "HandBrake", 0.5, 1e-6), 1, 1e-6)
		Test.ExpectEqual(Support.RoundTrip(rig, "Gear", 2), 0)
		Test.ExpectNear(Support.RoundTrip(rig, "Clutch", 0.5, 1e-6), 1, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "MaxTorque", 600, 1e-3), 500, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "MinRpm", 900, 1e-3), 1000, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "MaxRpm", 5000, 1e-3), 6000, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "EngineInertia", 0.6, 1e-6), 0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "EngineDamping", 0.25, 1e-6), 0.2, 1e-6)
		local curve = rig.TorqueCurve
		rig.TorqueCurve = { { X = 0, Y = 0.5 }, { X = 1, Y = 1 } }
		expectKeys(rig.TorqueCurve, { { X = 0, Y = 0.5 }, { X = 1, Y = 1 } }, "TorqueCurve reads back what was written")
		expectKeys(curve, { { X = 0, Y = 0.8 }, { X = 0.66, Y = 1 }, { X = 1, Y = 0.8 } }, "the rig's TorqueCurve is the default")
		Test.ExpectEqual(Support.RoundTrip(rig, "Transmission", "Manual"), "Auto")
		local gears = rig.GearRatios
		rig.GearRatios = { 3, 1.5 }
		expectNumbers(rig.GearRatios, { 3, 1.5 }, "GearRatios reads back what was written")
		expectNumbers(gears, { 2.66, 1.78, 1.3, 1, 0.74 }, "the rig's GearRatios are the default")
		local reverse = rig.ReverseGearRatios
		rig.ReverseGearRatios = { -2 }
		expectNumbers(rig.ReverseGearRatios, { -2 }, "ReverseGearRatios reads back what was written")
		expectNumbers(reverse, { -2.9 }, "the rig's ReverseGearRatios are the default")
		Test.ExpectNear(Support.RoundTrip(rig, "SwitchTime", 0.4, 1e-6), 0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "ClutchReleaseTime", 0.25, 1e-6), 0.3, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "SwitchLatency", 0.3, 1e-6), 0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "ShiftUpRpm", 3500, 1e-3), 4000, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "ShiftDownRpm", 1500, 1e-3), 2000, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "ClutchStrength", 8, 1e-6), 10, 1e-6)
		local differentials = rig.Differentials
		Test.ExpectEqual(#differentials, 1)
		Test.ExpectEqual(differentials[1].LeftWheel, rl)
		Test.ExpectEqual(differentials[1].RightWheel, rr)
		rig.Differentials = {
			{ LeftWheel = fl, RightWheel = fr, Ratio = 3, LeftRightSplit = 0.4, LimitedSlipRatio = 2, EngineTorqueRatio = 1 },
		}
		local written = rig.Differentials
		Test.ExpectEqual(#written, 1)
		Test.ExpectEqual(written[1].LeftWheel, fl)
		Test.ExpectEqual(written[1].RightWheel, fr)
		Test.ExpectNear(written[1].Ratio, 3, 1e-6)
		Test.ExpectNear(written[1].LeftRightSplit, 0.4, 1e-6)
		Test.ExpectNear(written[1].LimitedSlipRatio, 2, 1e-6)
		Test.ExpectNear(written[1].EngineTorqueRatio, 1, 1e-6)
		Test.ExpectNear(Support.RoundTrip(rig, "DifferentialLimitedSlipRatio", 0, 1e-6), 1.4, 1e-6)
		Test.ExpectEqual(#rig.AntiRollBars, 0)
		rig.AntiRollBars = { { LeftWheel = rl, RightWheel = rr, Stiffness = 500 } }
		local bars = rig.AntiRollBars
		Test.ExpectEqual(#bars, 1)
		Test.ExpectEqual(bars[1].LeftWheel, rl)
		Test.ExpectEqual(bars[1].RightWheel, rr)
		Test.ExpectNear(bars[1].Stiffness, 500, 1e-3)
		Test.ExpectNear(Support.RoundTrip(rig, "MaxPitchRollAngle", 45, 1e-4), 60, 1e-4)
		Test.ExpectEqual(Support.RoundTrip(rig, "WheelCast", "Ray"), "Sphere")

		local tyre = wheel("/Rig/FL")
		Test.ExpectNear(Support.RoundTrip(tyre, "Radius", 0.45, 1e-6), 0.4, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "Width", 0.3, 1e-6), 0.2, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "SuspensionMinLength", 0.25, 1e-6), 0.3, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "SuspensionMaxLength", 0.55, 1e-6), 0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "SuspensionPreload", 0.02, 1e-6), 0.05, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "SpringFrequency", 1.8, 1e-6), 1.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "SpringDamping", 0.4, 1e-6), 0.5, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "MaxSteerAngle", 25, 1e-5), 30, 1e-5)
		Test.ExpectNear(Support.RoundTrip(tyre, "MaxBrakeTorque", 1200, 1e-3), 1500, 1e-3)
		Test.ExpectNear(Support.RoundTrip(tyre, "MaxHandBrakeTorque", 3000, 1e-3), 0, 1e-3)
		Test.ExpectNear(Support.RoundTrip(tyre, "Inertia", 1, 1e-6), 0.9, 1e-6)
		Test.ExpectNear(Support.RoundTrip(tyre, "AngularDamping", 0.15, 1e-6), 0.2, 1e-6)
		local grip = tyre.LongitudinalFriction
		tyre.LongitudinalFriction = { { X = 0, Y = 0 }, { X = 0.1, Y = 1.1 } }
		expectKeys(tyre.LongitudinalFriction, { { X = 0, Y = 0 }, { X = 0.1, Y = 1.1 } }, "LongitudinalFriction reads back what was written")
		expectKeys(grip, { { X = 0, Y = 0 }, { X = 0.08, Y = 1.3 }, { X = 0.3, Y = 1.1 } }, "the rig's LongitudinalFriction")
		local side = tyre.LateralFriction
		tyre.LateralFriction = { { X = 0, Y = 0 }, { X = 5, Y = 1.1 } }
		expectKeys(tyre.LateralFriction, { { X = 0, Y = 0 }, { X = 5, Y = 1.1 } }, "LateralFriction reads back what was written")
		expectKeys(side, { { X = 0, Y = 0 }, { X = 4, Y = 1.3 }, { X = 25, Y = 1.1 } }, "the rig's LateralFriction")

		-- The registry's ranges hold on every write path.
		Test.Expect(not pcall(function()
			rig.Throttle = 2
		end), "a Throttle above 1 is refused")
		Test.Expect(not pcall(function()
			rig.Gear = 9
		end), "a gear beyond eight is refused")
		Test.Expect(not pcall(function()
			tyre.MaxSteerAngle = 95
		end), "a steer beyond 90 degrees is refused")
		Test.Expect(not pcall(function()
			mass.InertiaScale = vector.create(0, 1, 1)
		end), "an inertia scale below 0.01 is refused")

		-- The writes take effect at the next physics pre-step: the rig is made again, Manual in second gear.
		Test.WaitTicks(2)
		Test.ExpectEqual(rig:GetGear(), 2, "the rig drives in the gear written")
		Test.ExpectNear(tyre:GetSteerAngle(), 0.75 * 25, 1e-3, "the front left wheel steers by the written lock")
	end, { TimeoutTicks = 60 })
end, { CaseTimeoutTicks = 900 })
