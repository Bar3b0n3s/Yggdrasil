--!strict
-- Terrain.scene's Ball (M26; Docs/Decisions/0037-m26-contract.md decisions 21 and 25): the MoveX action pushes it along
-- X, out of the bowl that terrain.edit lowered into the imported ramp and up the slope, so Terrain.replay's bodies depend
-- on its input. Without input the ball stays in the bowl.
export type TerrainRoller = ScriptInstance & {
	Pushes: number,
}

local PUSH = 3 -- newtons per unit of MoveX

local TerrainRoller = {}

function TerrainRoller.OnCreate(self: TerrainRoller)
	self.Pushes = 0
end

function TerrainRoller.OnFixedUpdate(self: TerrainRoller, _dt: number)
	local move = Input.GetAxis("MoveX")
	if move ~= 0 then
		self.Pushes += 1
		local body = self.Entity.RigidBody :: RigidBody
		body:AddForce(vector.create(move * PUSH, 0, 0))
	end
end

return Script.Define("TerrainRoller", TerrainRoller)
