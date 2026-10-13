--!strict
export type Mover = ScriptInstance & {
	Speed: number,
}

local Mover = {}

Mover.Fields = {
	Speed = Field.Number(1, { Min = 0 }),
}

function Mover.OnFixedUpdate(self: Mover, dt: number)
	self.Entity.Transform:Translate(vector.create(self.Speed * dt, 0, 0))
end

return Script.Define("Mover", Mover)
