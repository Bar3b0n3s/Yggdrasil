--!strict
-- The health of GraphMembers.scene's Crystal (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 12; World Tree's
-- brief 18): Damage(hit) asks every other behaviour of its entity Blocks(hit) and, when none blocks the hit, takes its
-- Amount and calls Damaged(hit) on each behaviour that has it, as the game's Health does. The Rune graph beside it answers
-- both through its graph methods.
export type Health = ScriptInstance & {
	MaxHealth: number,
	Health: number,
}

local Health = {}

Health.Fields = {
	MaxHealth = Field.Number(10),
}

function Health.OnStart(self: Health)
	self.Health = self.MaxHealth
end

-- Whether a behaviour of the entity blocked `hit`.
function Health.Damage(self: Health, hit: any): boolean
	for _, behaviour in self.Entity:GetScripts() do
		-- The cast drops the checker's nil refinement of the member, under which it cannot be called.
		if not rawequal(behaviour, self) and behaviour.Blocks ~= nil and (behaviour :: any):Blocks(hit) then
			return true
		end
	end
	self.Health -= hit.Amount
	for _, behaviour in self.Entity:GetScripts() do
		if not rawequal(behaviour, self) and behaviour.Damaged ~= nil then
			(behaviour :: any):Damaged(hit)
		end
	end
	return false
end

return Script.Define("Health", Health)
