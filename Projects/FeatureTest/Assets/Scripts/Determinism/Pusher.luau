--!strict
-- Determinism.scene's scripted forces (§15.5): every fixed step it pushes its Targets with forces from script-side
-- DetMath trigonometry and two non-trivial powers of the authored Exponent, one through math.pow (DetMath) and one
-- through the ^ operator, steers the Ball with the MoveX axis and lifts it on Jump, and turns the kinematic Paddle.
-- Luau's ^ is exact only for the exponents 2, 3 and 0.5; for any other exponent it calls the C runtime's pow. Here the
-- exponent is a field value, so the compiler cannot fold it, and the replay's committed hash, which the determinism
-- stage verifies in the Debug and Release editors (§4.12, §15.8), covers that path.
export type Pusher = ScriptInstance & {
	Strength: number,
	Frequency: number,
	Exponent: number,
	Targets: { Entity },
	Ball: Entity?,
	Paddle: Entity?,
	Steps: number,
	Jumps: number,
	Work: number,
}

local Pusher = {}

Pusher.Fields = {
	Strength = Field.Number(4, { Min = 0, Max = 100, Tooltip = "Base force in newtons" }),
	Frequency = Field.Number(1, { Min = 0, Max = 10, Tooltip = "Turns of the force direction per second" }),
	Exponent = Field.Number(1.5, { Tooltip = "Exponent of the per-target force scale" }),
	Targets = Field.Array(Field.Entity()),
	Ball = Field.Entity(),
	Paddle = Field.Entity(),
}

function Pusher.OnCreate(self: Pusher)
	self.Steps = 0
	self.Jumps = 0
	self.Work = 0
end

function Pusher.OnFixedUpdate(self: Pusher, dt: number)
	self.Steps += 1
	local time = Time.GetTime()
	local phase = time * self.Frequency * 2 * math.pi
	for index, target in self.Targets do
		local body = target.RigidBody
		if body ~= nil then
			local angle = phase + index * 0.7
			local scale = math.pow(1 + (index % 5) * 0.37, self.Exponent)
			local spread = (1 + (index % 3) * 0.29) ^ self.Exponent
			local lift = 0.4 * math.abs(math.cos(angle * 0.5))
			local force = vector.create(math.sin(angle), lift, math.cos(angle)) * (self.Strength * (scale + 0.25 * spread))
			body:AddForce(force)
			self.Work += vector.magnitude(force) * dt
		end
	end
	local ball = self.Ball
	if ball ~= nil then
		local body = ball.RigidBody :: RigidBody
		local heading = math.atan2(Input.GetAxis("MoveX"), 1)
		body:AddTorque(vector.create(math.sin(heading) * 0.6, 0, -math.tan(heading) * 0.8))
		if Input.IsActionPressed("Jump") then
			body:AddImpulse(vector.create(0, 2.5, 0))
			self.Jumps += 1
		end
	end
	local paddle = self.Paddle
	if paddle ~= nil then
		local degrees = math.deg(math.asin(math.sin(phase * 0.25))) * 1.5
		local position = paddle.Transform.WorldPosition
		local body = paddle.RigidBody :: RigidBody
		body:MoveKinematic(position, Quat.FromEuler(0, degrees, 0))
	end
end

return Script.Define("Pusher", Pusher)
