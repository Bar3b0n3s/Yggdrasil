--!strict
-- A stub of the rolling-ball behaviour the documentation's examples require; only its types matter.
export type Ball = ScriptInstance & {
	Torque: number,
	-- Degrees, 0 facing -Z and positive turning left (Transform.EulerAngles y), kept in the fixed step; the control is
	-- relative to it, and the follow camera reads it.
	Heading: number,
	ReachedGoal: boolean,
}

local Ball = {}

Ball.Fields = {
	Torque = Field.Number(30, { Min = 0 }),
}

function Ball.OnStart(self: Ball)
	self.Heading = 0
	self.ReachedGoal = false
end

return Script.Define("Ball", Ball)
