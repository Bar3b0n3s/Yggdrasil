--!strict
-- CharacterMoves.scene's Swimmer (Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md; Skerries' brief
-- S17): swims along X at the MoveX axis times Speed with free moves that hold its feet Depth metres under a swell computed
-- from the session's tick (a sine standing in for M47's water), and walks with Move where the bottom is higher than that.
export type Swimmer = ScriptInstance & {
	Depth: number,
	Speed: number,
	SwellHeight: number,
	MaxRise: number,
	-- The sea's surface where the swimmer's next update takes it, and whether that update swims.
	Surface: number,
	Swimming: boolean,
}

local Swimmer = {}

Swimmer.Fields = {
	Depth = Field.Number(1.3, { Min = 0 }),
	Speed = Field.Number(4, { Min = 0 }),
	SwellHeight = Field.Number(0.15, { Min = 0 }),
	MaxRise = Field.Number(3, { Min = 0 }),
}

-- The sea's surface after `ticks` fixed steps: a swell of `height` metres about y 0.
local function surfaceAt(height: number, ticks: number): number
	return height * math.sin(ticks * Time.GetFixedDeltaTime() * 0.9)
end

function Swimmer.OnCreate(self: Swimmer)
	self.Surface = 0
	self.Swimming = false
end

function Swimmer.OnFixedUpdate(self: Swimmer, dt: number)
	local character = self.Entity.CharacterController
	if character == nil then
		return
	end
	local horizontal = vector.create(Input.GetAxis("MoveX") * self.Speed, 0, 0)
	-- This tick's update ends the step that makes the session's tick count one higher.
	self.Surface = surfaceAt(self.SwellHeight, Time.GetSessionTick() + 1)
	local target = self.Surface - self.Depth
	local y = self.Entity.Transform.WorldPosition.y
	-- Standing on a bottom higher than the depth it holds: a walker in the shallows.
	self.Swimming = not (character:IsGrounded() and y >= target)
	if self.Swimming then
		local rise = math.clamp((target - y) / dt, -self.MaxRise, self.MaxRise)
		character:MoveFree(horizontal + vector.create(0, rise, 0))
	else
		character:Move(horizontal)
	end
end

return Script.Define("Swimmer", Swimmer)
