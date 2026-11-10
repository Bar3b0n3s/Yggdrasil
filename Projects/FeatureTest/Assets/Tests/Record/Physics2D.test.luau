--!strict
-- The Physics2D replay's recording suite (Docs/Decisions/0032-m21-contract.md decision 17; Modes Editor): a planned input
-- list pushes the Ball right into the crate and through the box sensor, kicks it up and pushes it back, so every 2D
-- body it reaches and the sensors' events depend on the input. It only reads the scene and injects input.
-- Replays2D.jsonl records the replay over the same input with input.record around a play.step.
local Contacts2DScript = require("../../Scripts/Physics2D/Contacts2D")
local PusherScript = require("../../Scripts/Physics2D/Pusher")

type Step = { Tick: number, Action: string, State: InputEventState?, Value: number? }
local Plan: { Step } = {
	{ Tick = 8, Action = "MoveX", Value = 1 },
	{ Tick = 70, Action = "Jump", State = "Tap" },
	{ Tick = 110, Action = "MoveX", Value = -1 },
	{ Tick = 150, Action = "Jump", State = "Tap" },
	{ Tick = 190, Action = "MoveX", Value = 0 },
}

return Test.Suite("Record.Physics2D", function()
	Test.Case("pushes the ball through the sensor and back", function()
		local found = Scene.FindByName("Ball")
		Test.Expect(found ~= nil, "Physics2D.scene has a Ball")
		local ball = found :: Entity
		local tick = 0
		for _, step in Plan do
			Test.WaitTicks(step.Tick - tick)
			tick = step.Tick
			Test.InjectAction(step.Action, step.State, step.Value)
		end
		Test.WaitTicks(260 - tick)
		local pusher = ball:GetScript("Pusher") :: PusherScript.Pusher
		Test.ExpectEqual(pusher.Kicks, 2, "both kicks were pressed")
		local sensor = (Scene.FindByName("BoxSensor") :: Entity):GetScript("Contacts2D") :: Contacts2DScript.Contacts2D
		Test.Expect(#sensor.Events >= 1, "the pushed ball reached the box sensor")
	end)
end)
