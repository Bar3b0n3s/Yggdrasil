--!strict
-- Tilemaps.scene's Ball (Docs/Decisions/0032-m21-contract.md decisions 25 and 32): the MoveX action pushes it along X,
-- across the seams of the ground's merged boxes, and each press of Jump kicks it up, so the Tilemap replay's bodies and
-- the goal's trigger events depend on its input.
export type Roller = ScriptInstance & {
	Jumps: number,
}

local PUSH = 6 -- newtons per unit of MoveX
local KICK = 6 -- newton seconds

local Roller = {}

function Roller.OnCreate(self: Roller)
	self.Jumps = 0
end

function Roller.OnFixedUpdate(self: Roller, _dt: number)
	local body = self.Entity.RigidBody2D :: RigidBody2D
	local move = Input.GetAxis("MoveX")
	if move ~= 0 then
		body:AddForce(vector.create(move * PUSH, 0, 0))
	end
	if Input.IsActionPressed("Jump") then
		self.Jumps += 1
		body:AddImpulse(vector.create(0, KICK, 0))
	end
end

return Script.Define("Roller", Roller)
