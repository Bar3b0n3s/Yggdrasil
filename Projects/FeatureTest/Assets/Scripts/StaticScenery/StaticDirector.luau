--!strict
-- Drives StaticScenery.scene (M43, Docs/Decisions/0061-m43-static-scenery.md decision 3; StaticScenery.jsonl, IdSeed
-- 4001): while LiftTicks is above 0 it raises the static Lift a quarter metre a tick, a write to static scenery that
-- rebuilds the Lift's cell at the next extracted frame; Fire rolls the Ball into a static rock at 4 m/s and starts six
-- such ticks (StaticScenery.replay). Without input or a suite's LiftTicks nothing moves.
export type StaticDirector = ScriptInstance & {
	LiftTicks: number,
	Lifts: number,
	Pushes: number,
}

local StaticDirector = {}

function StaticDirector.OnCreate(self: StaticDirector)
	self.LiftTicks = 0
	self.Lifts = 0
	self.Pushes = 0
end

function StaticDirector.OnFixedUpdate(self: StaticDirector, dt: number)
	if Input.IsActionPressed("Fire") then
		local ball = Scene.FindByPath("/Ball")
		local body = if ball then ball.RigidBody else nil
		if body then
			body:SetLinearVelocity(vector.create(0, 0, -4))
			self.Pushes += 1
			self.LiftTicks += 6
		end
	end
	if self.LiftTicks > 0 then
		self.LiftTicks -= 1
		local lift = Scene.FindByPath("/Scenery/Lift")
		if lift then
			lift.Transform.Translation += vector.create(0, 0.25, 0)
			self.Lifts += 1
		end
	end
end

return Script.Define("StaticDirector", StaticDirector)
