--!strict
-- The Sprites2D replay's recording suite (Docs/Decisions/0032-m21-contract.md decision 17; Modes Editor): a planned input
-- list walks Strider 2.5 units right, jumps, walks 1.25 units back left at half speed, stops and jumps again, so its
-- position, facing, frames and clips depend on the input. It only reads the scene and injects input.
-- Replays2D.jsonl records the replay over the same input with input.record around a play.step.
local StriderScript = require("../../Scripts/Sprites2D/Strider")

type Step = { Tick: number, Action: string, State: InputEventState?, Value: number? }
local Plan: { Step } = {
	{ Tick = 6, Action = "MoveX", Value = 1 },
	{ Tick = 46, Action = "MoveX", Value = 0 },
	{ Tick = 60, Action = "Jump", State = "Tap" },
	{ Tick = 90, Action = "MoveX", Value = -0.5 },
	{ Tick = 130, Action = "MoveX", Value = 0 },
	{ Tick = 170, Action = "Jump", State = "Tap" },
}

return Test.Suite("Record.Sprites2D", function()
	Test.Case("walks, jumps, turns and stops", function()
		local found = Scene.FindByName("Strider")
		Test.Expect(found ~= nil, "Sprites2D.scene has a Strider")
		local strider = found :: Entity
		local tick = 0
		for _, step in Plan do
			Test.WaitTicks(step.Tick - tick)
			tick = step.Tick
			Test.InjectAction(step.Action, step.State, step.Value)
		end
		Test.WaitTicks(240 - tick)
		local script = strider:GetScript() :: StriderScript.Strider
		Test.ExpectEqual(script.Jumps, 2, "both jumps were pressed")
		Test.Expect(strider.Transform.Translation.x > 1, "the walk moved Strider right of its start")
		Test.ExpectEqual((strider.SpriteRenderer :: SpriteRenderer).FlipX, true, "Strider last walked left")
	end)
end)
