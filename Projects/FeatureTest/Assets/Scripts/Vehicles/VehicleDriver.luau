--!strict
-- The Buggy's driver in Vehicles.scene (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Skerries' S02): W and S
-- drive forward and backward (Auto reverses for a negative Throttle), A and D steer left and right, and Space pulls the hand
-- brake. It writes the WheeledVehicle's driver input in the fixed step while one of its keys is held, and once more when the
-- last one is released, so the Vehicles replay drives the buggy from planned input while the Vehicles suite's cases, which
-- hold no key, drive it by writing the fields themselves. It counts the ticks it drove and the fastest speed it read, which
-- the replay's expectations check.
export type VehicleDriver = ScriptInstance & {
	Driving: boolean,
	DrivenTicks: number,
	TopSpeed: number,
}

local VehicleDriver = {}

local function axis(positive: Key, negative: Key): number
	return (if Input.IsKeyDown(positive) then 1 else 0) - (if Input.IsKeyDown(negative) then 1 else 0)
end

function VehicleDriver.OnCreate(self: VehicleDriver)
	self.Driving = false
	self.DrivenTicks = 0
	self.TopSpeed = 0
end

function VehicleDriver.OnFixedUpdate(self: VehicleDriver, _dt: number)
	local vehicle = self.Entity.WheeledVehicle
	if vehicle == nil then
		return
	end
	local held = Input.IsKeyDown("W") or Input.IsKeyDown("S") or Input.IsKeyDown("A") or Input.IsKeyDown("D") or Input.IsKeyDown("Space")
	if not held and not self.Driving then
		return
	end
	vehicle.Throttle = axis("W", "S")
	vehicle.Steering = axis("D", "A")
	vehicle.HandBrake = if Input.IsKeyDown("Space") then 1 else 0
	self.Driving = held
	if held then
		self.DrivenTicks += 1
		self.TopSpeed = math.max(self.TopSpeed, vehicle:GetSpeed())
	end
end

return Script.Define("VehicleDriver", VehicleDriver)
