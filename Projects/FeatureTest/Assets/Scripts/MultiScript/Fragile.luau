--!strict
-- A script slot that faults in its next fixed step once a test arms it (MultiScript.scene's Hero): the fault disables
-- this slot's instance only, and the entity's other slots keep running (Docs/Decisions/0031-m20-contract.md decision 17).
export type Fragile = ScriptInstance & {
	Armed: boolean,
	Steps: number,
}

local Fragile = {}

function Fragile.OnCreate(self: Fragile)
	self.Armed = false
	self.Steps = 0
end

function Fragile.OnFixedUpdate(self: Fragile, dt: number)
	self.Steps += 1
	if self.Armed then
		self.Armed = false
		error("deliberate slot fault")
	end
end

return Script.Define("Fragile", Fragile)
