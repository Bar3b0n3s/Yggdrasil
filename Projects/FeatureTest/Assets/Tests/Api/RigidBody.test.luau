--!strict
-- The RigidBody proxy (§9.3, §11.5): forces, impulses, velocities, teleports, sleep and kinematic moves reach the
-- body, and every field reads and writes.
local Support = require("./Support")

local FIXED = 1 / 64

return Test.Suite("Api.RigidBody", function()
	Test.Case("velocities, impulses and forces move a dynamic body", function()
		Test.WaitTicks(1) -- bodies appear at the first step's PreStep
		local ball = Support.Find("Ball")
		local body = ball.RigidBody :: RigidBody
		Physics.SetGravity(vector.zero)
		body:SetLinearVelocity(vector.create(1, 2, 3))
		body:SetAngularVelocity(vector.create(0, 1, 0))
		Test.ExpectEqual(body:GetLinearVelocity(), vector.create(1, 2, 3))
		Test.ExpectEqual(body:GetAngularVelocity(), vector.create(0, 1, 0))
		body:SetLinearVelocity(vector.zero)
		body:SetAngularVelocity(vector.zero)
		body:AddImpulse(vector.create(2, 0, 0))
		Test.WaitTicks(1)
		Test.ExpectNear(body:GetLinearVelocity().x, 2, 0.01, "an impulse of 2 N s on 1 kg")
		body:SetLinearVelocity(vector.zero)
		body:AddForce(vector.create(0, 0, 64))
		Test.WaitTicks(1)
		Test.ExpectNear(body:GetLinearVelocity().z, 64 * FIXED, 0.01, "a force acts for one step")
		body:SetLinearVelocity(vector.zero)
		body:AddAngularImpulse(vector.create(0, 0.1, 0))
		Test.WaitTicks(1)
		Test.Expect(body:GetAngularVelocity().y > 0, "an angular impulse spins the body")
		body:SetAngularVelocity(vector.zero)
		body:AddTorque(vector.create(1, 0, 0))
		Test.WaitTicks(1)
		Test.Expect(body:GetAngularVelocity().x > 0, "a torque spins the body")
		body:SetAngularVelocity(vector.zero)
		body:AddForceAtPosition(vector.create(0, 10, 0), ball.Transform.WorldPosition + vector.create(0.5, 0, 0))
		Test.WaitTicks(1)
		Test.Expect(body:GetLinearVelocity().y > 0 and body:GetAngularVelocity().z > 0, "an off-centre force pushes and spins")
	end)

	Test.Case("teleports, sleep and kinematic moves", function()
		local ball = Support.Find("Ball")
		local body = ball.RigidBody :: RigidBody
		body:Teleport(vector.create(-2, 6, 2), Quat.FromEuler(0, 90, 0))
		Test.ExpectEqual(ball.Transform.WorldPosition, vector.create(-2, 6, 2))
		Test.ExpectEqual(type(body:IsSleeping()), "boolean")
		body:WakeUp()
		Test.ExpectEqual(body:IsSleeping(), false)
		local platform = Support.Find("Platform")
		local kinematic = platform.RigidBody :: RigidBody
		kinematic:MoveKinematic(vector.create(4, 1.5, 0), Quat.Identity())
		Test.WaitTicks(1)
		Test.ExpectNear(platform.Transform.WorldPosition, vector.create(4, 1.5, 0), 0.001, "the kinematic body reached its target")
		Test.Expect(not pcall(function()
			kinematic:AddForce(vector.one)
		end), "forces need a dynamic body")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local body = Support.Find("Crate").RigidBody :: RigidBody
		Support.RoundTrip(body, "Mass", 2.5, 1e-6)
		Support.RoundTrip(body, "Friction", 0.8, 1e-6)
		Support.RoundTrip(body, "Restitution", 0.4, 1e-6)
		Support.RoundTrip(body, "LinearDamping", 0.1, 1e-6)
		Support.RoundTrip(body, "AngularDamping", 0.2, 1e-6)
		Support.RoundTrip(body, "GravityFactor", 0.5, 1e-6)
		for _, quality: MotionQuality in { "LinearCast", "Discrete" } do
			Support.RoundTrip(body, "MotionQuality", quality)
		end
		Test.ExpectEqual(Support.RoundTrip(body, "AllowSleeping", true), false, "the scene keeps the crate awake")
		local locks = body.LockTranslation
		Test.ExpectEqual(table.concat({ tostring(locks[1]), tostring(locks[2]), tostring(locks[3]) }, ","), "false,false,false")
		body.LockTranslation = { true, false, true }
		Test.ExpectEqual(body.LockTranslation[1], true)
		Test.ExpectEqual(body.LockTranslation[3], true)
		body.LockTranslation = { false, false, false }
		body.LockRotation = { false, true, false }
		Test.ExpectEqual(body.LockRotation[2], true)
		body.LockRotation = { false, false, false }
		Test.ExpectEqual(Support.RoundTrip(body, "Layer", "Ball"), "Default")
		Support.RoundTrip(body, "MaxLinearVelocity", 100, 1e-4)
		Support.RoundTrip(body, "MaxAngularVelocity", 120, 1e-4)
		Support.RoundTrip(body, "EnhancedInternalEdgeRemoval", true)
		Support.RoundTrip(body, "InitialLinearVelocity", vector.create(0, 1, 0))
		Support.RoundTrip(body, "InitialAngularVelocity", vector.create(0, 0, 1))
		for _, kind: BodyType in { "Kinematic", "Static", "Dynamic" } do
			Support.RoundTrip(body, "Type", kind)
		end
		Test.WaitTicks(1)
		Test.Expect(not pcall(function()
			body.Mass = 0
		end), "a zero mass is refused")
	end)
end)
