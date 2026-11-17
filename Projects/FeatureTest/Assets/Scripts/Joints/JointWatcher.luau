--!strict
-- The welded plank's behaviour in Joints.scene (M37, Docs/Decisions/0054-m37-joints.md decisions 8 and 12): OnJointBreak
-- records the break the hammer causes, its force and torque and the tick, which the Joints suite and replay read, and the
-- weld's own readings until then.
export type JointWatcher = ScriptInstance & {
	Breaks: number,
	BrokeAt: number,
	Force: number,
	Torque: number,
	BrokenWhenCalled: boolean,
	HeaviestLoad: number,
}

local JointWatcher = {}

function JointWatcher.OnCreate(self: JointWatcher)
	self.Breaks = 0
	self.BrokeAt = -1
	self.Force = 0
	self.Torque = 0
	self.BrokenWhenCalled = false
	self.HeaviestLoad = 0
end

function JointWatcher.OnFixedUpdate(self: JointWatcher, dt: number)
	local weld = self.Entity.FixedJoint
	if weld ~= nil and not weld.Broken then
		self.HeaviestLoad = math.max(self.HeaviestLoad, weld:GetForce())
	end
end

function JointWatcher.OnJointBreak(self: JointWatcher, force: number, torque: number)
	self.Breaks += 1
	self.BrokeAt = Time.GetTick()
	self.Force = force
	self.Torque = torque
	local weld = self.Entity.FixedJoint
	self.BrokenWhenCalled = weld ~= nil and weld.Broken
end

return Script.Define("JointWatcher", JointWatcher)
