--!strict
-- The native Bumper system, driven through the Bump action: Main.scene's Bumper rests at 0.5 m, climbs 0.5 m per
-- press and drops back after 4 presses. Luau cannot read native components, so the cases read the Transform the
-- system writes.

local function Height(): number
	local found = Scene.FindByName("Bumper")
	Test.Expect(found ~= nil, "Main.scene has a Bumper entity")
	return (found :: Entity).Transform.Translation.y
end

local function Press()
	Test.InjectAction("Bump", "Tap")
	Test.WaitTicks(5)
end

return Test.Suite("Bumper", function()
	Test.Case("rests at its base height without input", function()
		Test.WaitTicks(60)
		Test.ExpectNear(Height(), 0.5, 0.0001, "the Bumper stayed at its base height")
	end)

	Test.Case("climbs one step per press", function()
		Press()
		Test.ExpectNear(Height(), 1.0, 0.0001, "one press raised it one step")
		Press()
		Test.ExpectNear(Height(), 1.5, 0.0001, "two presses raised it two steps")
	end)

	Test.Case("counts a held Bump once", function()
		Test.InjectAction("Bump", "Down")
		Test.WaitTicks(30)
		Test.InjectAction("Bump", "Up")
		Test.WaitTicks(5)
		Test.ExpectNear(Height(), 1.0, 0.0001, "holding Bump for 30 ticks is one press")
	end)

	Test.Case("drops back after its last level", function()
		Press()
		Press()
		Press()
		Test.ExpectNear(Height(), 2.0, 0.0001, "three presses reach the top level")
		Press()
		Test.ExpectNear(Height(), 0.5, 0.0001, "the fourth press drops it back to its base height")
	end)
end)
