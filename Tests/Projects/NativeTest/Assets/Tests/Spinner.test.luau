--!strict
-- The native Spinner system, driven through the Spin action: Main.scene's Spinner turns 90 deg/s about Y and the
-- Wheel -45 deg/s about its own Y while Spin is held. Luau cannot read native components, so the cases read the
-- Transform the system writes.

local function Yaw(name: string): number
	local found = Scene.FindByName(name)
	Test.Expect(found ~= nil, `Main.scene has a {name} entity`)
	return (found :: Entity).Transform.EulerAngles.y
end

return Test.Suite("Spinner", function()
	Test.Case("stays still without input", function()
		Test.WaitTicks(60)
		Test.ExpectNear(Yaw("Spinner"), 0, 0.0001, "the Spinner did not turn")
		Test.ExpectNear(Yaw("Wheel"), 0, 0.0001, "the Wheel did not turn")
	end)

	Test.Case("turns at its speed while Spin is held", function()
		Test.InjectAction("Spin", "Down")
		Test.WaitTicks(60)
		Test.InjectAction("Spin", "Up")
		Test.WaitTicks(2)
		Test.ExpectNear(Yaw("Spinner"), 90, 0.01, "the Spinner turned 90 degrees in one second")
		Test.ExpectNear(Yaw("Wheel"), -45, 0.01, "the Wheel turned -45 degrees in one second")
	end)

	Test.Case("stops turning when Spin is released", function()
		Test.InjectAction("Spin", "Down")
		Test.WaitTicks(30)
		Test.InjectAction("Spin", "Up")
		Test.WaitTicks(2)
		local released = Yaw("Spinner")
		Test.Expect(released > 40, "the Spinner turned while Spin was held")
		Test.WaitTicks(60)
		Test.ExpectEqual(Yaw("Spinner"), released, "the Spinner keeps its angle once Spin is released")
	end)

	Test.Case("wraps its angle after a full turn", function()
		Test.InjectAction("Spin", "Down")
		Test.WaitTicks(250)
		Test.InjectAction("Spin", "Up")
		Test.WaitTicks(2)
		Test.ExpectNear(Yaw("Spinner"), 15, 0.01, "250 ticks at 1.5 degrees each is 375 degrees, one turn and 15")
	end, { TimeoutTicks = 400 })
end)
