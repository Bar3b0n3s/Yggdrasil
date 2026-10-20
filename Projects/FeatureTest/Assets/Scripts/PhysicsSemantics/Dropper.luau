--!strict
-- Moves its ball between a resting place on the floor and a drop point above BrickB on every press of the Jump action,
-- so that the PhysicsSemantics replay's collider events depend on its input (Docs/Decisions/0031-m20-contract.md
-- decision 2).

export type Dropper = ScriptInstance & {
	Dropped: boolean,
	Presses: number,
}

local DROP_POINT = vector.create(-6, 3, 0.5)
local REST_POINT = vector.create(-6, 0.25, 4)

local Dropper = {}

function Dropper.OnCreate(self: Dropper)
	self.Dropped = false
	self.Presses = 0
end

function Dropper.OnFixedUpdate(self: Dropper, _dt: number)
	if not Input.IsActionPressed("Jump") then
		return
	end
	self.Presses += 1
	self.Dropped = not self.Dropped
	local body = self.Entity.RigidBody :: RigidBody
	body:Teleport(if self.Dropped then DROP_POINT else REST_POINT)
	body:SetLinearVelocity(vector.zero)
end

return Script.Define("Dropper", Dropper)
