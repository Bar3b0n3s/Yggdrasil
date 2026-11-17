--!strict
-- Joints (M37, Docs/Decisions/0054-m37-joints.md decisions 8 and 12; World Tree's brief 16, its first part) in Joints.scene
-- (Joints.jsonl, IdSeed 3400), in every run mode: a chain of four links on distance joints under a static beam swings when
-- struck and comes to rest; a trap door on a motored, limited hinge drops what stands on it and closes again; a piston on
-- a limited slider runs to its limits under its velocity motor; a weld breaks under the hammer and calls OnJointBreak; a
-- joint authored broken is mended; every method of the four joint types, every field through the proxy, and the errors of a
-- joint that is not in force. Each case starts from a fresh load of the scene (Isolation "Case").
local function find(path: string): Entity
	local entity = Scene.FindByPath(path)
	assert(entity, `{path} is in the scene`)
	return entity
end

local function hinge(path: string): HingeJoint
	local joint = find(path).HingeJoint
	assert(joint, `{path} has a HingeJoint`)
	return joint
end

local function slider(path: string): SliderJoint
	local joint = find(path).SliderJoint
	assert(joint, `{path} has a SliderJoint`)
	return joint
end

local function distance(path: string): DistanceJoint
	local joint = find(path).DistanceJoint
	assert(joint, `{path} has a DistanceJoint`)
	return joint
end

local function fixed(path: string): FixedJoint
	local joint = find(path).FixedJoint
	assert(joint, `{path} has a FixedJoint`)
	return joint
end

local function body(path: string): RigidBody
	local component = find(path).RigidBody
	assert(component, `{path} has a RigidBody`)
	return component
end

local function height(path: string): number
	return find(path).Transform.WorldPosition.y
end

local function speed(path: string): number
	return vector.magnitude(body(path):GetLinearVelocity())
end

return Test.Suite("Joints", function()
	Test.Case("the chain hangs from the beam, swings when struck and comes to rest", function()
		Test.WaitTicks(64)
		for index = 0, 3 do
			local link = distance(`/Chain/Link{index}`)
			Test.Expect(link:GetDistance() <= link.MaxDistance + 0.02, `link {index} stays within its rope`)
			Test.Expect(link:GetForce() > 0, `link {index} carries the links below it`)
		end
		-- The top link carries the whole chain's weight, more than the bottom one.
		Test.Expect(distance("/Chain/Link0"):GetForce() > distance("/Chain/Link3"):GetForce(), "the top link carries the most")
		body("/Chain/Link3"):AddImpulse(vector.create(3, 0, 1.5))
		Test.WaitTicks(8)
		Test.Expect(speed("/Chain/Link3") > 0.5, "the struck link swings")
		Test.Expect(speed("/Chain/Link1") > 0.05, "the swing reaches up the chain")
		Test.WaitTicks(640)
		for index = 0, 3 do
			Test.Expect(speed(`/Chain/Link{index}`) < 0.05, `link {index} comes to rest`)
		end
		Test.ExpectNear(find("/Chain/Link3").Transform.WorldPosition.x, 0, 0.05)
	end, { TimeoutTicks = 900 })

	Test.Case("the trap door drops what stands on it and closes again", function()
		Test.WaitTicks(32)
		local door = hinge("/TrapDoor")
		Test.ExpectNear(door:GetAngle(), 0, 1)
		local crateHeight = height("/Crate")
		Test.Expect(crateHeight > 2, "the crate rests on the closed door")
		Test.Expect(door:GetForce() > 0, "the hinge holds the door and the crate up")
		Test.Expect(door:GetTorque() >= 0, "the torque is a magnitude (none here: the load turns the door only about its own axis, which the motor holds)")
		door.MotorTargetAngle = -50
		Test.WaitTicks(64)
		Test.Expect(door:GetAngle() < -40, "the door opens down to its target")
		Test.Expect(door:GetAngle() >= -61, "and no further than its limit")
		Test.WaitTicks(64)
		Test.Expect(height("/Crate") < crateHeight - 1, "the crate fell through")
		door.MotorTargetAngle = 0
		Test.WaitTicks(128)
		Test.ExpectNear(door:GetAngle(), 0, 1)
		Test.Expect(door:GetForce() > 0, "the hinge holds the closed door up")
	end)

	Test.Case("the piston runs to its limits under its velocity motor", function()
		local piston = slider("/Piston")
		Test.WaitTicks(96)
		Test.ExpectNear(piston:GetPosition(), piston.UpperLimit, 0.02)
		Test.Expect(piston:GetForce() > 0, "the rail holds the piston")
		Test.Expect(piston:GetTorque() >= 0, "the torque is a magnitude")
		piston.MotorTargetVelocity = -piston.MotorTargetVelocity
		Test.WaitTicks(160)
		Test.ExpectNear(piston:GetPosition(), piston.LowerLimit, 0.02)
	end)

	Test.Case("the weld breaks under the hammer and calls OnJointBreak", function()
		local plank = find("/Plank")
		local weld = fixed("/Plank")
		local watcher = plank:GetScript("JointWatcher")
		assert(watcher, "the plank watches its weld")
		Test.WaitTicks(16)
		local resting = weld:GetForce()
		Test.Expect(resting > 0 and resting < weld.BreakForce / 4, "the plank's own weight is far below the weld's limit")
		Test.Expect(weld:GetTorque() > 0, "the weld holds the plank level")
		Test.ExpectEqual(watcher.Breaks, 0)
		Test.InjectKey("H", "Tap")
		Test.WaitTicks(96)
		Test.ExpectEqual(watcher.Breaks, 1)
		Test.Expect(watcher.Force > weld.BreakForce or watcher.Torque > weld.BreakTorque, "it broke past a threshold")
		Test.Expect(watcher.BrokenWhenCalled, "Broken is true when OnJointBreak runs")
		Test.Expect(weld.Broken, "the weld stays broken")
		Test.Expect(height("/Plank") < 1.5, "the plank fell")
		Test.Expect(not pcall(function()
			return weld:GetForce()
		end), "a broken joint has no force to read")
	end)

	Test.Case("a joint authored broken joins nothing until Broken is written false", function()
		local bucket = fixed("/Bucket")
		Test.WaitTicks(4)
		Test.ExpectEqual(bucket.Broken, true)
		Test.Expect(not pcall(function()
			return bucket:GetForce()
		end), "a broken joint is not in force")
		bucket.Broken = false
		Test.WaitTicks(2)
		Test.Expect(bucket:GetForce() >= 0, "the mended joint is in force")
		Test.ExpectEqual(bucket:GetTorque() >= 0, true)
		body("/Bucket"):AddImpulse(vector.create(0, 4, 0))
		Test.WaitTicks(32)
		Test.ExpectNear(height("/Bucket"), 0.25, 0.05)
	end)

	Test.Case("every joint field reads and writes through its proxy", function()
		local spareHinge = hinge("/Spares/HingeSpare")
		spareHinge.Anchor = vector.create(0.1, 0.2, 0.3)
		Test.ExpectEqual(spareHinge.Anchor, vector.create(0.1, 0.2, 0.3))
		spareHinge.Axis = vector.create(0, 1, 0)
		Test.ExpectEqual(spareHinge.Axis, vector.create(0, 1, 0))
		spareHinge.UseLimits = not spareHinge.UseLimits
		spareHinge.LowerAngle = -45
		spareHinge.UpperAngle = 30
		spareHinge.MaxFrictionTorque = 4
		spareHinge.Motor = "Velocity"
		spareHinge.MotorTargetVelocity = 20
		spareHinge.MotorTargetAngle = 15
		spareHinge.MotorMaxTorque = 300
		spareHinge.MotorFrequency = 5
		spareHinge.MotorDamping = 0.5
		spareHinge.CollideConnected = true
		spareHinge.BreakForce = 50
		spareHinge.BreakTorque = 60
		spareHinge.ConnectedBody = find("/Beam")
		Test.ExpectEqual(spareHinge.LowerAngle, -45)
		Test.ExpectEqual(spareHinge.UpperAngle, 30)
		Test.ExpectEqual(spareHinge.MaxFrictionTorque, 4)
		Test.ExpectEqual(spareHinge.Motor, "Velocity")
		Test.ExpectEqual(spareHinge.MotorTargetVelocity, 20)
		Test.ExpectEqual(spareHinge.MotorTargetAngle, 15)
		Test.ExpectEqual(spareHinge.MotorMaxTorque, 300)
		Test.ExpectEqual(spareHinge.MotorFrequency, 5)
		Test.ExpectEqual(spareHinge.MotorDamping, 0.5)
		Test.ExpectEqual(spareHinge.CollideConnected, true)
		Test.ExpectEqual(spareHinge.BreakForce, 50)
		Test.ExpectEqual(spareHinge.BreakTorque, 60)
		Test.ExpectEqual(spareHinge.ConnectedBody, find("/Beam"))
		Test.ExpectEqual(spareHinge.Broken, true)
		spareHinge.Broken = true

		local spareSlider = slider("/Spares/SliderSpare")
		spareSlider.Anchor = vector.create(0, 0.1, 0)
		spareSlider.Axis = vector.create(0, 0, 1)
		spareSlider.UseLimits = true
		spareSlider.LowerLimit = -2
		spareSlider.UpperLimit = 3
		spareSlider.MaxFrictionForce = 6
		spareSlider.Motor = "Position"
		spareSlider.MotorTargetVelocity = -1
		spareSlider.MotorTargetPosition = 0.75
		spareSlider.MotorMaxForce = 80
		spareSlider.MotorFrequency = 3
		spareSlider.MotorDamping = 0.25
		spareSlider.CollideConnected = false
		spareSlider.BreakForce = 70
		spareSlider.BreakTorque = 90
		spareSlider.ConnectedBody = nil
		Test.ExpectEqual(spareSlider.Anchor, vector.create(0, 0.1, 0))
		Test.ExpectEqual(spareSlider.Axis, vector.create(0, 0, 1))
		Test.ExpectEqual(spareSlider.UseLimits, true)
		Test.ExpectEqual(spareSlider.LowerLimit, -2)
		Test.ExpectEqual(spareSlider.UpperLimit, 3)
		Test.ExpectEqual(spareSlider.MaxFrictionForce, 6)
		Test.ExpectEqual(spareSlider.Motor, "Position")
		Test.ExpectEqual(spareSlider.MotorTargetVelocity, -1)
		Test.ExpectEqual(spareSlider.MotorTargetPosition, 0.75)
		Test.ExpectEqual(spareSlider.MotorMaxForce, 80)
		Test.ExpectEqual(spareSlider.MotorFrequency, 3)
		Test.ExpectEqual(spareSlider.MotorDamping, 0.25)
		Test.ExpectEqual(spareSlider.CollideConnected, false)
		Test.ExpectEqual(spareSlider.BreakForce, 70)
		Test.ExpectEqual(spareSlider.BreakTorque, 90)
		Test.ExpectEqual(spareSlider.ConnectedBody, nil)
		Test.ExpectEqual(spareSlider.Broken, true)
		spareSlider.Broken = true

		local spareRope = distance("/Spares/DistanceSpare")
		spareRope.Anchor = vector.create(0, -0.1, 0)
		spareRope.ConnectedAnchor = vector.create(1, 2, 3)
		spareRope.MinDistance = 0.5
		spareRope.MaxDistance = 2.5
		spareRope.Frequency = 2
		spareRope.Damping = 0.125
		spareRope.CollideConnected = true
		spareRope.BreakForce = 25
		spareRope.ConnectedBody = find("/Beam")
		Test.ExpectEqual(spareRope.Anchor, vector.create(0, -0.1, 0))
		Test.ExpectEqual(spareRope.ConnectedAnchor, vector.create(1, 2, 3))
		Test.ExpectEqual(spareRope.MinDistance, 0.5)
		Test.ExpectEqual(spareRope.MaxDistance, 2.5)
		Test.ExpectEqual(spareRope.Frequency, 2)
		Test.ExpectEqual(spareRope.Damping, 0.125)
		Test.ExpectEqual(spareRope.CollideConnected, true)
		Test.ExpectEqual(spareRope.BreakForce, 25)
		Test.ExpectEqual(spareRope.ConnectedBody, find("/Beam"))
		Test.ExpectEqual(spareRope.Broken, true)
		spareRope.Broken = true

		local bucket = fixed("/Bucket")
		bucket.Anchor = vector.create(0, 0.2, 0)
		bucket.CollideConnected = true
		bucket.BreakForce = 1000
		bucket.BreakTorque = 2000
		bucket.ConnectedBody = find("/Ground")
		bucket.Broken = true
		Test.ExpectEqual(bucket.Anchor, vector.create(0, 0.2, 0))
		Test.ExpectEqual(bucket.CollideConnected, true)
		Test.ExpectEqual(bucket.BreakForce, 1000)
		Test.ExpectEqual(bucket.BreakTorque, 2000)
		Test.ExpectEqual(bucket.ConnectedBody, find("/Ground"))
		Test.ExpectEqual(bucket.Broken, true)
		-- The registry's ranges hold on every write path.
		Test.Expect(not pcall(function()
			spareHinge.UpperAngle = 200
		end), "an angle beyond a turn is refused")
		Test.Expect(not pcall(function()
			spareSlider.MotorFrequency = 0
		end), "a position drive needs a spring above 0 Hz")
		Test.Expect(not pcall(function()
			spareRope.MinDistance = -1
		end), "a distance is not negative")
	end)

	Test.Case("a joint that is not in force has no reading", function()
		local trap = hinge("/TrapDoor")
		trap.Broken = true
		Test.WaitTicks(1)
		Test.Expect(not pcall(function()
			return trap:GetAngle()
		end), "a joint written broken is not in force")
		Test.Expect(not pcall(function()
			return slider("/Spares/SliderSpare"):GetPosition()
		end), "nor is a spare authored broken")
		trap.Broken = false
		Test.WaitTicks(1)
		Test.ExpectNear(trap:GetAngle(), 0, 1)
	end)
end)
