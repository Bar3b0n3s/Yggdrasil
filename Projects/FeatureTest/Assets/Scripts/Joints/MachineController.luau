--!strict
-- The machines of Joints.scene (M37, Docs/Decisions/0054-m37-joints.md decision 12; World Tree's brief 16): keys drive them
-- in the fixed step, so the Joints replay plays them from planned input. S strikes the chain's last link, O opens the trap
-- door and C closes it, H drops the hammer onto the welded plank (it hangs still until then, its GravityFactor 0). It counts
-- what it did, so the replay's expectations and the Joints suite read it.
export type MachineController = ScriptInstance & {
	Strikes: number,
	Opened: number,
	Closed: number,
	Dropped: boolean,
}

local MachineController = {}

function MachineController.OnCreate(self: MachineController)
	self.Strikes = 0
	self.Opened = 0
	self.Closed = 0
	self.Dropped = false
end

function MachineController.OnFixedUpdate(self: MachineController, dt: number)
	if Input.IsKeyPressed("S") then
		local link = Scene.FindByPath("/Chain/Link3")
		if link ~= nil and link.RigidBody ~= nil then
			link.RigidBody:AddImpulse(vector.create(3, 0, 1.5))
			self.Strikes += 1
		end
	end
	local door = Scene.FindByPath("/TrapDoor")
	if door ~= nil and door.HingeJoint ~= nil then
		if Input.IsKeyPressed("O") then
			door.HingeJoint.MotorTargetAngle = -50
			self.Opened += 1
		elseif Input.IsKeyPressed("C") then
			door.HingeJoint.MotorTargetAngle = 0
			self.Closed += 1
		end
	end
	if Input.IsKeyPressed("H") and not self.Dropped then
		local hammer = Scene.FindByPath("/Hammer")
		if hammer ~= nil and hammer.RigidBody ~= nil then
			hammer.RigidBody.GravityFactor = 1
			hammer.RigidBody:AddImpulse(vector.create(0, -60, 0))
			self.Dropped = true
		end
	end
end

return Script.Define("MachineController", MachineController)
