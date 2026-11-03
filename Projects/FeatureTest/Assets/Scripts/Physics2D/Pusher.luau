--!strict
-- Physics2D.scene's Ball (Docs/Decisions/0032-m21-contract.md decisions 9 and 17): the MoveX action pushes it along X
-- and each press of Jump kicks it up, so the Physics2D replay's bodies depend on its input.
export type Pusher = ScriptInstance & {
	Kicks: number,
}

local PUSH = 30 -- newtons per unit of MoveX
local KICK = 14 -- newton seconds

local Pusher = {}

function Pusher.OnCreate(self: Pusher)
	self.Kicks = 0
end

function Pusher.OnFixedUpdate(self: Pusher, _dt: number)
	local body = self.Entity.RigidBody2D :: RigidBody2D
	local move = Input.GetAxis("MoveX")
	if move ~= 0 then
		body:AddForce(vector.create(move * PUSH, 0, 0))
	end
	if Input.IsActionPressed("Jump") then
		self.Kicks += 1
		body:AddImpulse(vector.create(0, KICK, 0))
	end
end

return Script.Define("Pusher", Pusher)
