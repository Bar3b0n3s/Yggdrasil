--!strict
-- The frozen constants of Rolling Ball's control, roll sound and respawns (ADR 0038 decision 2).
local Tuning = table.freeze({
	-- The ground probe: a ray from the ball's centre straight down over this many metres on the Track layer.
	GroundProbe = 0.65,
	-- The heading turns towards the horizontal velocity while the ball is grounded and faster than this (m/s).
	HeadingMinSpeed = 2.5,
	-- The most the heading turns, in degrees per second.
	HeadingRate = 120,
	-- The torque along the velocity fades to nothing between this fraction of MaxSpeed and MaxSpeed.
	SpeedFadeStart = 0.8,
	-- The roll sound: silent below RollMinSpeed, loudest RollSpeedRange m/s above it.
	RollMinSpeed = 0.5,
	RollSpeedRange = 10,
	RollMaxVolume = 0.6,
	-- The roll sound's pitch: RollPitchBase plus RollPitchPerSpeed a metre per second, at most RollPitchMax.
	RollPitchBase = 0.7,
	RollPitchPerSpeed = 0.05,
	RollPitchMax = 1.6,
	-- Volume and pitch are written in steps of this size, only when the step changes.
	RollStep = 0.05,
	-- A respawn places the ball's centre this far above the checkpoint's root.
	RespawnLift = 0.6,
	-- Once the goal is reached the ball brakes with this fraction of its torque.
	GoalBrake = 1,
})

return Tuning
