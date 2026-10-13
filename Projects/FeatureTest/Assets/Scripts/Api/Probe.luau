--!strict
-- A behaviour declaring a field of every Field kind, so the Scripting scene can override each one (Script.Fields) and
-- the Api suites can read them back through the instance and the Script component proxy.
export type Probe = ScriptInstance & {
	Speed: number,
	Count: number,
	Enabled: boolean,
	Title: string,
	Offset: vector,
	Tint: Color,
	Facing: Quat,
	Target: Entity?,
	Clip: AssetRef?,
	Mode: string,
	Waypoints: { number },
	Created: boolean,
	Starts: number,
	Steps: number,
}

local Probe = {}

Probe.Fields = {
	Speed = Field.Number(1.5, { Min = 0, Max = 10, Step = 0.5, Tooltip = "Metres per second" }),
	Count = Field.Integer(3, { Min = 0, Max = 100 }),
	Enabled = Field.Bool(false),
	Title = Field.String("Probe", { Tooltip = "A display name" }),
	Offset = Field.Vector(vector.create(0, 1, 0)),
	Tint = Field.Color(Color.New(1, 0.5, 0.25)),
	Facing = Field.Quat(Quat.Identity()),
	Target = Field.Entity(),
	Clip = Field.Asset("AudioClip"),
	Mode = Field.Enum({ "Idle", "Patrol", "Chase" }, "Patrol"),
	Waypoints = Field.Array(Field.Number()),
}

function Probe.OnCreate(self: Probe)
	self.Created = true
	self.Starts = 0
	self.Steps = 0
end

function Probe.OnStart(self: Probe)
	self.Starts += 1
end

function Probe.OnFixedUpdate(self: Probe, dt: number)
	self.Steps += 1
end

return Script.Define("Probe", Probe)
