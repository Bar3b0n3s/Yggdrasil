--!strict
-- A stub of the rolling-ball behaviour the documentation's examples require; only its types matter.
export type Ball = ScriptInstance & {
	Torque: number,
	ReachedGoal: boolean,
}

local Ball = {}

Ball.Fields = {
	Torque = Field.Number(30, { Min = 0 }),
}

function Ball.OnStart(self: Ball)
	self.ReachedGoal = false
end

return Script.Define("Ball", Ball)
