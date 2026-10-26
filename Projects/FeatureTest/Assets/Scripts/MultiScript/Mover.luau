--!strict
-- Moves its entity along X by the MoveX action (MultiScript.scene's Pilot, slot 0). The Trail of the next slot reads the
-- position this slot wrote in the same fixed step (Docs/Decisions/0031-m20-contract.md decision 17).
export type Mover = ScriptInstance & {
	Speed: number,
}

local Mover = {}

Mover.Fields = {
	Speed = Field.Number(1, { Min = 0, Max = 20, Tooltip = "Metres per second at full MoveX" }),
}

function Mover.OnFixedUpdate(self: Mover, dt: number)
	self.Entity.Transform:Translate(vector.create(Input.GetAxis("MoveX") * self.Speed * dt, 0, 0))
end

return Script.Define("Mover", Mover)
