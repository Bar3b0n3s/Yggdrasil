--!strict
export type FixtureBehaviour = ScriptInstance & {
	Speed: number,
}

local FixtureBehaviour = {}

FixtureBehaviour.Fields = {
	Speed = Field.Number(1, { Min = 0, Tooltip = "Movement speed." }),
}

function FixtureBehaviour.OnFixedUpdate(self: FixtureBehaviour, dt: number)
	self.Entity.Transform:Translate(vector.create(self.Speed * dt, 0, 0))
end

return Script.Define("FixtureBehaviour", FixtureBehaviour)
