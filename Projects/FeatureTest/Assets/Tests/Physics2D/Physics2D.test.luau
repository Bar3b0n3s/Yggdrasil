--!strict
-- Physics2D.scene (Docs/Decisions/0032-m21-contract.md decisions 8 to 11 and 13): the 2D world starts at the project's
-- Physics2D.Gravity, keeps every body in its plane, and delivers OnCollisionEnter2D, OnCollisionExit2D,
-- OnTriggerEnter2D and OnTriggerExit2D to both sides of a pair, each enter before its exit, in every run mode.
local Contacts2DScript = require("../../Scripts/Physics2D/Contacts2D")
local Support = require("../Api/Support")

-- The kinds of `entity`'s recorded events with `other`, in arrival order.
local function kinds(entity: Entity, other: string): string
	local recorder = entity:GetScript("Contacts2D") :: Contacts2DScript.Contacts2D
	local found: { string } = {}
	for _, event in recorder.Events do
		if event.Other == other then
			table.insert(found, event.Kind)
		end
	end
	return table.concat(found, ",")
end

return Test.Suite("Physics2D", function()
	Test.Case("the 2D world starts at the project's gravity and keeps its bodies in the plane", function()
		Test.ExpectEqual(Physics2D.GetGravity(), vector.create(0, -12, 0))
		Test.WaitTicks(90)
		for _, name in { "Ball", "Crate", "Wedge" } do
			local entity = Support.Find(name)
			Test.ExpectEqual(entity.Transform.WorldPosition.z, 0, name .. " stays on z = 0")
			local rotation = entity.Transform.WorldRotation
			Test.ExpectEqual(rotation.x, 0, name .. " turns about Z only")
			Test.ExpectEqual(rotation.y, 0, name .. " turns about Z only")
		end
		Test.Expect(Support.Find("Ball").Transform.WorldPosition.x > -8, "the ball rolled off at its initial velocity")
		Test.ExpectNear(Support.Find("Lift").Transform.WorldPosition, vector.create(8, 2, 0), 0, "an unmoved kinematic body stays")
	end)

	Test.Case("a resting ball enters the floor and leaves it when lifted", function()
		local ball = Support.Find("Ball")
		Test.WaitTicks(2)
		Test.ExpectEqual(kinds(ball, "Floor"), "CollisionEnter2D")
		Test.ExpectEqual(kinds(Support.Find("Floor"), "Ball"), "CollisionEnter2D", "both sides are called")
		local recorder = ball:GetScript("Contacts2D") :: Contacts2DScript.Contacts2D
		local normal = recorder.Events[1].Normal :: vector
		Test.ExpectEqual(normal.z, 0, "the contact normal lies in the plane")
		Test.ExpectNear(math.abs(normal.y), 1, 1e-3, "the floor pushes along Y")
		local body = ball.RigidBody2D :: RigidBody2D
		body:Teleport(vector.create(-8, 9, 0))
		Test.WaitTicks(2)
		Test.ExpectEqual(kinds(ball, "Floor"), "CollisionEnter2D,CollisionExit2D")
		Test.ExpectEqual(kinds(Support.Find("Floor"), "Ball"), "CollisionEnter2D,CollisionExit2D")
	end)

	Test.Case("a ball passing through a sensor enters and leaves it", function()
		local ball = Support.Find("Ball")
		local body = ball.RigidBody2D :: RigidBody2D
		for _, sensor in { "BoxSensor", "CircleSensor", "WedgeSensor" } do
			local zone = Support.Find(sensor)
			body:Teleport(zone.Transform.WorldPosition)
			body:SetLinearVelocity(vector.zero)
			Test.WaitTicks(2)
			Test.ExpectEqual(kinds(zone, "Ball"), "TriggerEnter2D", sensor .. " sees the ball enter")
			Test.ExpectEqual(kinds(ball, sensor), "TriggerEnter2D", "the ball sees " .. sensor)
			body:Teleport(vector.create(0, 12, 0))
			Test.WaitTicks(2)
			Test.ExpectEqual(kinds(zone, "Ball"), "TriggerEnter2D,TriggerExit2D", sensor .. " sees the ball leave")
			Test.ExpectEqual(kinds(ball, sensor), "TriggerEnter2D,TriggerExit2D")
		end
	end)
end)
