--!strict
-- Surfaces.scene's piston and press (Docs/Decisions/0053-m36-contact-surfaces.md): moves its Kinematic RigidBody from where
-- it starts along Direction at Speed for Distance metres, then holds it there.
export type KinematicMover = ScriptInstance & {
	Direction: vector,
	Speed: number,
	Distance: number,
	Start: vector,
	Travelled: number,
}

local KinematicMover = {}

KinematicMover.Fields = {
	Direction = Field.Vector(vector.create(1, 0, 0)),
	Speed = Field.Number(2, { Min = 0 }),
	Distance = Field.Number(1, { Min = 0 }),
}

function KinematicMover.OnStart(self: KinematicMover)
	self.Start = self.Entity.Transform.WorldPosition
	self.Travelled = 0
end

function KinematicMover.OnFixedUpdate(self: KinematicMover, dt: number)
	local body = self.Entity.RigidBody
	if body == nil or self.Travelled >= self.Distance then
		return
	end
	self.Travelled = math.min(self.Distance, self.Travelled + self.Speed * dt)
	body:MoveKinematic(self.Start + self.Direction * self.Travelled, self.Entity.Transform.WorldRotation)
end

return Script.Define("KinematicMover", KinematicMover)
