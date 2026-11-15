--!strict
-- The follow camera: in the frame phase it follows the ball's interpolated RenderPosition from behind along the
-- ball's Heading, which the fixed step keeps. Presentation only: it writes the camera's Transform and nothing the
-- simulation reads.
local BallScript = require("./Ball")
local Motion = require("./Motion")

export type FollowCamera = ScriptInstance & {
	Ball: Entity?,
	Distance: number,
	Height: number,
	SmoothTime: number,
	Velocity: vector,
	SeenTeleports: number,
}

local FollowCamera = {}

FollowCamera.Fields = {
	Ball = Field.Entity(),
	Distance = Field.Number(7, { Min = 0 }),
	Height = Field.Number(2.8),
	SmoothTime = Field.Number(0.15, { Min = 0.01 }),
}

local LookLift = vector.create(0, 0.6, 0)

function FollowCamera.OnStart(self: FollowCamera)
	self.Velocity = vector.zero
	self.SeenTeleports = -1
end

function FollowCamera.OnLateUpdate(self: FollowCamera, dt: number)
	local ballEntity = self.Ball
	if not ballEntity or not ballEntity:IsValid() then
		return
	end
	local ball = ballEntity:GetScript("Ball") :: BallScript.Ball?
	if not ball or ball.Heading == nil then
		return
	end
	local focus = ballEntity.Transform.RenderPosition
	local target = focus - Motion.Forward(ball.Heading) * self.Distance + vector.create(0, self.Height, 0)
	local transform = self.Entity.Transform
	if ball.Teleports ~= self.SeenTeleports then
		-- A spawn or a respawn: snap behind the ball instead of sweeping across the level.
		self.SeenTeleports = ball.Teleports
		self.Velocity = vector.zero
		transform.WorldPosition = target
	else
		local position, velocity = Math.SmoothDamp(transform.WorldPosition, target, self.Velocity, self.SmoothTime, dt)
		self.Velocity = velocity
		transform.WorldPosition = position
	end
	transform:LookAt(focus + LookLift)
end

return Script.Define("FollowCamera", FollowCamera)
