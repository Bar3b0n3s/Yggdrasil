--!strict
local Jumper = {}

function Jumper.OnFixedUpdate(self: ScriptInstance, dt: number)
	if Input.IsActionPressed("Jump") then
		self.Entity.Transform:Translate(vector.create(0, dt, 0))
	end
end

return Script.Define("Jumper", Jumper)
