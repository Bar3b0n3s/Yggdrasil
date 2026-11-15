--!strict
-- A kinematic platform that dwells, eases along Travel, dwells and returns, counted in ticks (ADR 0038 decision 2).
-- Its authored pose is the cycle's start pose; PhaseTicks starts it that far into the cycle.
local Motion = require("./Motion")

export type MovingPlatform = ScriptInstance & {
	Travel: vector,
	MoveTicks: number,
	DwellTicks: number,
	PhaseTicks: number,
	Tick: number,
	Start: vector,
	StartRotation: Quat,
	AtStart: boolean,
	AtEnd: boolean,
	DwellLeft: number,
}

local MovingPlatform = {}

MovingPlatform.Fields = {
	Travel = Field.Vector(vector.create(0, 4, 0)),
	MoveTicks = Field.Integer(180, { Min = 1 }),
	DwellTicks = Field.Integer(120, { Min = 0 }),
	PhaseTicks = Field.Integer(0, { Min = 0 }),
}

local function updatePhase(self: MovingPlatform)
	local atStart, atEnd, dwellLeft = Motion.Dwell(self.Tick, self.MoveTicks, self.DwellTicks)
	self.AtStart = atStart
	self.AtEnd = atEnd
	self.DwellLeft = dwellLeft
end

function MovingPlatform.OnStart(self: MovingPlatform)
	local transform = self.Entity.Transform
	self.Start = transform.WorldPosition
	self.StartRotation = transform.WorldRotation
	self.Tick = self.PhaseTicks
	local body = self.Entity.RigidBody
	if body and self.PhaseTicks ~= 0 then
		-- Start at the phased pose, so the first tick moves one tick's step instead of jumping there and launching
		-- whatever rides the platform.
		body:Teleport(self.Start + self.Travel * Motion.Ease(self.Tick, self.MoveTicks, self.DwellTicks), self.StartRotation)
	end
	updatePhase(self)
end

function MovingPlatform.OnFixedUpdate(self: MovingPlatform, _dt: number)
	local body = self.Entity.RigidBody
	if not body then
		return
	end
	self.Tick += 1
	local progress = Motion.Ease(self.Tick, self.MoveTicks, self.DwellTicks)
	body:MoveKinematic(self.Start + self.Travel * progress, self.StartRotation)
	updatePhase(self)
end

return Script.Define("MovingPlatform", MovingPlatform)
