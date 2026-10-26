--!strict
-- Stretches its entity along X by its distance from the origin (MultiScript.scene's Pilot, slot 1). It runs after the
-- Mover of slot 0 in every fixed step (slot order), so the scale always matches the position of the same step, which the
-- state hash and the MultiScript replay's checkpoints see (Docs/Decisions/0031-m20-contract.md decision 17).
export type Trail = ScriptInstance & {
	Gain: number,
	Samples: number,
}

local Trail = {}

Trail.Fields = {
	Gain = Field.Number(0.5, { Min = 0, Max = 10, Tooltip = "Scale added per metre from the origin" }),
}

function Trail.OnCreate(self: Trail)
	self.Samples = 0
end

function Trail.OnFixedUpdate(self: Trail, dt: number)
	self.Samples += 1
	local transform = self.Entity.Transform
	transform.Scale = vector.create(1 + math.abs(transform.Translation.x) * self.Gain, 1, 1)
end

return Script.Define("Trail", Trail)
