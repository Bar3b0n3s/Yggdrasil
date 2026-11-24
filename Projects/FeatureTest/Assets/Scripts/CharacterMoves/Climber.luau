--!strict
-- CharacterMoves.scene's Climber (Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md; Skerries' brief
-- S17): on the Ladder trigger it climbs with free moves, pressed into the wall at Grip m/s and rising or coming down at the
-- MoveY axis times ClimbSpeed, and holds where it is in the air with no input; off the ladder it walks along X with Move,
-- E forwards and Q back, so at the top a forward walk steps off onto the platform.
export type Climber = ScriptInstance & {
	ClimbSpeed: number,
	WalkSpeed: number,
	Grip: number,
	OnLadder: boolean,
}

local Climber = {}

Climber.Fields = {
	ClimbSpeed = Field.Number(2, { Min = 0 }),
	WalkSpeed = Field.Number(2, { Min = 0 }),
	Grip = Field.Number(0.5, { Min = 0 }),
}

function Climber.OnCreate(self: Climber)
	self.OnLadder = false
end

function Climber.OnTriggerEnter(self: Climber, other: Entity)
	if other.Name == "Ladder" then
		self.OnLadder = true
	end
end

function Climber.OnTriggerExit(self: Climber, other: Entity)
	if other.Name == "Ladder" then
		self.OnLadder = false
	end
end

function Climber.OnFixedUpdate(self: Climber, dt: number)
	local character = self.Entity.CharacterController
	if character == nil then
		return
	end
	local up = Input.GetAxis("MoveY")
	if self.OnLadder and (up ~= 0 or not character:IsGrounded()) then
		character:MoveFree(vector.create(self.Grip, up * self.ClimbSpeed, 0))
		return
	end
	local forward = (if Input.IsKeyDown("E") then 1 else 0) - (if Input.IsKeyDown("Q") then 1 else 0)
	character:Move(vector.create(forward * self.WalkSpeed, 0, 0))
end

return Script.Define("Climber", Climber)
