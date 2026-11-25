--!strict
-- Turns Sky.scene's Sun while the action Fire is held: a camp's time-lapse, the sun sinking a degree a tick toward dusk and
-- night (M45; Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Sky.replay). Written in the fixed step, so the
-- turn is simulation state, hashed and replayed; without input the sun stands still, as the Sky suite's still check needs.
export type SunTurner = ScriptInstance & {
	-- The degrees the sun has turned.
	Turned: number,
}

-- How far the sun turns each tick the action is held, in degrees about its local X.
local DEGREES_PER_TICK = 1

local SunTurner = {}

function SunTurner.OnCreate(self: SunTurner)
	self.Turned = 0
end

function SunTurner.OnFixedUpdate(self: SunTurner, dt: number)
	if Input.IsActionDown("Fire") then
		self.Entity.Transform:Rotate(vector.create(DEGREES_PER_TICK, 0, 0))
		self.Turned += DEGREES_PER_TICK
	end
end

return Script.Define("SunTurner", SunTurner)
