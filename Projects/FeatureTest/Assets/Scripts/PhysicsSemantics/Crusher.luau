--!strict
-- The collision form of the read rule (Docs/Decisions/0031-m20-contract.md decision 4): Anvil and Hammer both run it.
-- A pair's enter reaches the entity with the lower id first, which destroys itself; the other one's callback, later in the
-- same dispatch, then reads the destroyed partner, which stays readable until the end-of-step flush.

export type Reading = { Ok: boolean, Name: string, Height: number, Valid: boolean, Tick: number }

export type Crusher = ScriptInstance & {
	Partner: string,
	Read: Reading?,
}

local Crusher = {}

Crusher.Fields = {
	Partner = Field.String("", { Tooltip = "The name of the entity this one crushes or is crushed by" }),
}

function Crusher.OnCollisionEnter(self: Crusher, other: Entity, _contact: CollisionContact)
	if other.Name ~= self.Partner or self.Read ~= nil then
		return
	end
	if self.Entity.ID < other.ID then
		Scene.Destroy(self.Entity)
		return
	end
	local ok, name, height = pcall(function(): (string, number)
		return other.Name, other.Transform.WorldPosition.y
	end)
	self.Read = {
		Ok = ok,
		Name = if ok then name else "",
		Height = if ok then height else 0,
		Valid = other:IsValid(),
		Tick = Time.GetTick(),
	}
end

return Script.Define("Crusher", Crusher)
