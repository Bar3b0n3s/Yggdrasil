--!strict
-- The RigidBody2D proxy (Docs/Decisions/0032-m21-contract.md decision 13): forces, impulses, velocities, teleports,
-- sleep and kinematic moves act in the plane, vectors' Z ignored and angles in degrees about +Z, and every field reads
-- and writes.
local Support = require("./Support")

local FIXED = 1 / 64

-- Places the Ball in free space with no gravity and no motion.
local function float(ball: Entity): RigidBody2D
	local body = ball.RigidBody2D :: RigidBody2D
	Physics2D.SetGravity(vector.zero)
	body:Teleport(vector.create(0, 8, 0), 0)
	body:SetLinearVelocity(vector.zero)
	body:SetAngularVelocity(0)
	return body
end

return Test.Suite("Api.RigidBody2D", function()
	Test.Case("velocities, impulses and forces move a dynamic body in the plane", function()
		Test.WaitTicks(1) -- bodies appear at the first step's pre-step
		local ball = Support.Find("Ball")
		local body = float(ball)
		body:SetLinearVelocity(vector.create(1, 2, 3))
		body:SetAngularVelocity(1.5)
		Test.ExpectEqual(body:GetLinearVelocity(), vector.create(1, 2, 0), "Z is ignored")
		Test.ExpectEqual(body:GetAngularVelocity(), 1.5)
		body:SetLinearVelocity(vector.zero)
		body:SetAngularVelocity(0)
		body:AddImpulse(vector.create(4, 0, 9))
		Test.WaitTicks(1)
		Test.ExpectNear(body:GetLinearVelocity(), vector.create(2, 0, 0), 0.05, "an impulse of 4 N s on 2 kg")
		body:SetLinearVelocity(vector.zero)
		body:AddForce(vector.create(0, 128, 0))
		Test.WaitTicks(1)
		Test.ExpectNear(body:GetLinearVelocity().y, 128 / 2 * FIXED, 0.05, "a force acts for one step")
		body:SetLinearVelocity(vector.zero)
		body:AddAngularImpulse(0.1)
		Test.WaitTicks(1)
		Test.Expect(body:GetAngularVelocity() > 0, "an angular impulse turns the body counter-clockwise")
		body:SetAngularVelocity(0)
		body:AddTorque(-2)
		Test.WaitTicks(1)
		Test.Expect(body:GetAngularVelocity() < 0, "a negative torque turns it clockwise")
		body:SetAngularVelocity(0)
		body:AddForceAtPosition(vector.create(0, 20, 0), ball.Transform.WorldPosition + vector.create(0.5, 0, 0))
		Test.WaitTicks(1)
		Test.Expect(body:GetLinearVelocity().y > 0 and body:GetAngularVelocity() > 0, "an off-centre force pushes and turns")
		Test.ExpectEqual(ball.Transform.WorldPosition.z, 0, "the body stays on its plane")
	end)

	Test.Case("teleports, sleep and kinematic moves", function()
		local ball = Support.Find("Ball")
		local body = float(ball)
		body:Teleport(vector.create(-2, 6, 4), 90)
		Test.ExpectEqual(ball.Transform.WorldPosition, vector.create(-2, 6, 0), "the entity keeps its Z")
		Support.ExpectQuatNear(ball.Transform.WorldRotation, Quat.AngleAxis(90, vector.create(0, 0, 1)), 1e-5, "the angle turns about +Z")
		body:Teleport(vector.create(-3, 6, 0))
		Support.ExpectQuatNear(ball.Transform.WorldRotation, Quat.AngleAxis(90, vector.create(0, 0, 1)), 1e-5, "an omitted angle is kept")
		Test.ExpectEqual(type(body:IsSleeping()), "boolean")
		body:WakeUp()
		Test.ExpectEqual(body:IsSleeping(), false)
		local lift = Support.Find("Lift")
		local kinematic = lift.RigidBody2D :: RigidBody2D
		kinematic:MoveKinematic(vector.create(8, 3, 0), 30)
		Test.WaitTicks(1)
		Test.ExpectNear(lift.Transform.WorldPosition, vector.create(8, 3, 0), 0.001, "the kinematic body reached its target")
		Support.ExpectQuatNear(lift.Transform.WorldRotation, Quat.AngleAxis(30, vector.create(0, 0, 1)), 1e-4, "and its angle")
		Test.Expect(not pcall(function()
			kinematic:AddForce(vector.one)
		end), "forces need a dynamic body")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local body = Support.Find("Crate").RigidBody2D :: RigidBody2D
		Support.RoundTrip(body, "Mass", 2.5, 1e-6)
		Support.RoundTrip(body, "Friction", 0.8, 1e-6)
		Support.RoundTrip(body, "Restitution", 0.4, 1e-6)
		Support.RoundTrip(body, "LinearDamping", 0.1, 1e-6)
		Support.RoundTrip(body, "AngularDamping", 0.2, 1e-6)
		Support.RoundTrip(body, "GravityFactor", 0.5, 1e-6)
		for _, quality: MotionQuality in { "LinearCast", "Discrete" } do
			Support.RoundTrip(body, "MotionQuality", quality)
		end
		Test.ExpectEqual(Support.RoundTrip(body, "AllowSleeping", false), true)
		Test.ExpectEqual(Support.RoundTrip(body, "FixedRotation", false), true, "the scene keeps the crate upright")
		Test.ExpectEqual(Support.RoundTrip(body, "Layer", "Ball"), "Default")
		Support.RoundTrip(body, "MaxLinearVelocity", 100, 1e-4)
		Support.RoundTrip(body, "MaxAngularVelocity", 120, 1e-4)
		Test.ExpectEqual(Support.RoundTrip(body, "EnhancedInternalEdgeRemoval", false), true)
		Support.RoundTrip(body, "InitialLinearVelocity", vector.create(0, 1, 0))
		Support.RoundTrip(body, "InitialAngularVelocity", 2, 1e-6)
		for _, kind: BodyType in { "Kinematic", "Static", "Dynamic" } do
			Support.RoundTrip(body, "Type", kind)
		end
		Test.WaitTicks(1)
		Test.Expect(not pcall(function()
			body.Mass = 0
		end), "a zero mass is refused")
	end)
end)
