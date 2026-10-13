--!strict
local Probe = {}

function Probe.OnFixedUpdate(self: ScriptInstance, dt: number)
	if Input.IsActionDown("Jump") then
		self.Entity.Transform:Translate(vector.create(0, dt, 0))
	end
	local axis = Input.GetAxis("MoveX")
	if axis ~= 0 then
		self.Entity.Transform:Translate(vector.create(axis * dt, 0, 0))
	end
end

return Script.Define("Probe", Probe)
