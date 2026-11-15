--!strict
-- The player's ball: heading-relative torque on the ground, a small force in the air, the kill plane and respawn,
-- the checkpoints and the goal, and the roll sound (ADR 0038 decision 2). Everything here runs in the fixed step.
local Actions = require("./Actions")
local Motion = require("./Motion")
local Tuning = require("./Tuning")

export type Ball = ScriptInstance & {
	Torque: number,
	AirForce: number,
	MaxSpeed: number,
	KillHeight: number,
	FallSound: AssetRef?,
	Heading: number,
	Grounded: boolean,
	LastOrder: number,
	Respawns: number,
	ReachedGoal: boolean,
	Teleports: number,
	Spawn: vector,
	SpawnHeading: number,
	RollVolumeStep: number,
	RollPitchStep: number,
	Respawn: (self: Ball) -> (),
}

local Ball = {}

Ball.Fields = {
	Torque = Field.Number(6, { Min = 0, Tooltip = "Rolling torque on the ground, N m" }),
	AirForce = Field.Number(3, { Min = 0, Tooltip = "Steering force in the air, N" }),
	MaxSpeed = Field.Number(11, { Min = 1, Tooltip = "The speed the forward torque fades out at, m/s" }),
	KillHeight = Field.Number(-8, { Tooltip = "Below this height the ball respawns" }),
	FallSound = Field.Asset("AudioClip"),
}

local Up = vector.create(0, 1, 0)
local Down = vector.create(0, -1, 0)

function Ball.OnStart(self: Ball)
	local transform = self.Entity.Transform
	self.Heading = Motion.Wrap(transform.EulerAngles.y)
	self.Grounded = false
	self.LastOrder = 0
	self.Respawns = 0
	self.ReachedGoal = false
	self.Teleports = 0
	self.Spawn = transform.WorldPosition
	self.SpawnHeading = self.Heading
	self.RollVolumeStep = -1
	self.RollPitchStep = -1
end

function Ball.Respawn(self: Ball)
	local body = self.Entity.RigidBody
	if not body then
		return
	end
	-- Teleport keeps the velocity: zero both.
	body:Teleport(self.Spawn, Quat.Identity())
	body:SetLinearVelocity(vector.zero)
	body:SetAngularVelocity(vector.zero)
	self.Heading = self.SpawnHeading
	self.Respawns += 1
	self.Teleports += 1
	local sound = self.FallSound
	if sound then
		Audio.PlayOneShot(sound)
	end
end

local function updateRollSound(self: Ball, speed: number)
	local audio = self.Entity.AudioSource
	if not audio then
		return
	end
	local volume = 0
	if self.Grounded then
		volume = Tuning.RollMaxVolume * math.clamp((speed - Tuning.RollMinSpeed) / Tuning.RollSpeedRange, 0, 1)
		local pitch = math.clamp(Tuning.RollPitchBase + Tuning.RollPitchPerSpeed * speed, Tuning.RollPitchBase, Tuning.RollPitchMax)
		local pitchStep = math.round(pitch / Tuning.RollStep)
		if pitchStep ~= self.RollPitchStep then
			self.RollPitchStep = pitchStep
			audio.Pitch = pitchStep * Tuning.RollStep
		end
	end
	local volumeStep = math.round(volume / Tuning.RollStep)
	if volumeStep ~= self.RollVolumeStep then
		self.RollVolumeStep = volumeStep
		audio.Volume = volumeStep * Tuning.RollStep
	end
end

function Ball.OnFixedUpdate(self: Ball, dt: number)
	local entity = self.Entity
	local body = entity.RigidBody
	if not body then
		return
	end
	local position = entity.Transform.WorldPosition
	if position.y < self.KillHeight then
		self:Respawn()
		return
	end

	self.Grounded = Physics.Raycast(position, Down, Tuning.GroundProbe, Physics.LayerMask("Track")) ~= nil
	local velocity = body:GetLinearVelocity()
	local flat = vector.create(velocity.x, 0, velocity.z)
	local flatSpeed = vector.magnitude(flat)
	if self.Grounded and flatSpeed > Tuning.HeadingMinSpeed then
		self.Heading = Motion.Turn(self.Heading, Motion.HeadingOf(flat), Tuning.HeadingRate * dt)
	end

	local move: vector
	if self.ReachedGoal then
		-- The run is over: the ball no longer takes input and brakes to a stop on the goal deck.
		move = if flatSpeed > 0.05 then -flat / flatSpeed * Tuning.GoalBrake else vector.zero
	else
		move = Motion.Right(self.Heading) * Input.GetAxis(Actions.MoveX) + Motion.Forward(self.Heading) * Input.GetAxis(Actions.MoveZ)
		local length = vector.magnitude(move)
		if length > 1 then
			move /= length
		end
	end

	if self.Grounded then
		-- The torque along the velocity fades out between SpeedFadeStart and MaxSpeed; braking always applies.
		if flatSpeed > 0.001 then
			local direction = flat / flatSpeed
			local along = vector.dot(move, direction)
			if along > 0 then
				local fade = Math.InverseLerp(Tuning.SpeedFadeStart * self.MaxSpeed, self.MaxSpeed, flatSpeed)
				move -= direction * along * fade
			end
		end
		body:AddTorque(vector.cross(Up, move) * self.Torque)
	else
		body:AddForce(move * self.AirForce)
	end

	updateRollSound(self, vector.magnitude(velocity))
end

function Ball.OnTriggerEnter(self: Ball, other: Entity)
	if other:HasTag("Checkpoint") then
		-- The checkpoint's graph behaviour holds its Order; graph members are typed any. Only a higher Order moves
		-- the spawn, so rolling back through an earlier checkpoint never moves it backwards.
		local checkpoint = other:GetScript("Checkpoint")
		local order = if checkpoint then checkpoint.Order else nil
		if type(order) == "number" and order > self.LastOrder then
			self.LastOrder = order
			self.Spawn = other.Transform.WorldPosition + vector.create(0, Tuning.RespawnLift, 0)
			self.SpawnHeading = Motion.Wrap(other.Transform.EulerAngles.y)
		end
	elseif other:HasTag("Goal") then
		self.ReachedGoal = true
	end
end

return Script.Define("Ball", Ball)
