--!strict
-- Moves its entity at a constant Velocity in every fixed step (Timing.scene's script-moved entities).
export type Mover = ScriptInstance & {
	Velocity: vector,
}

local Mover = {}

Mover.Fields = {
	Velocity = Field.Vector(vector.create(1, 0, 0), { Tooltip = "Metres per second, applied in the fixed phase" }),
}

function Mover.OnFixedUpdate(self: Mover, dt: number)
	local transform = self.Entity.Transform
	transform.Translation += self.Velocity * dt
end

return Script.Define("Mover", Mover)
