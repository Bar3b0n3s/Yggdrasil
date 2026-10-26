--!strict
-- The recording suite of Assets/Tests/Replays/SpinAndBump.replay: a planned input list that holds Spin for 90 ticks
-- and presses Bump three times, so the native systems turn the Spinner 135 degrees and raise the Bumper to its top
-- level. It only reads the scene and injects input.

type Step = { Tick: number, Action: string, State: "Down" | "Up" | "Tap" }

local Plan: { Step } = {
	{ Tick = 10, Action = "Spin", State = "Down" },
	{ Tick = 40, Action = "Bump", State = "Tap" },
	{ Tick = 70, Action = "Bump", State = "Tap" },
	{ Tick = 100, Action = "Spin", State = "Up" },
	{ Tick = 130, Action = "Bump", State = "Tap" },
}

return Test.Suite("Record.SpinAndBump", function()
	Test.Case("spins and bumps", function()
		local spinner = Scene.FindByName("Spinner")
		local bumper = Scene.FindByName("Bumper")
		Test.Expect(spinner ~= nil and bumper ~= nil, "Main.scene has a Spinner and a Bumper")
		local tick = 0
		for _, step in Plan do
			Test.WaitTicks(step.Tick - tick)
			tick = step.Tick
			Test.InjectAction(step.Action, step.State)
		end
		Test.WaitTicks(60)
		Test.ExpectNear((spinner :: Entity).Transform.EulerAngles.y, 135, 0.01, "holding Spin for 90 ticks turned the Spinner 135 degrees")
		Test.ExpectNear((bumper :: Entity).Transform.Translation.y, 2.0, 0.0001, "three presses of Bump raised the Bumper to its top level")
	end)
end)
