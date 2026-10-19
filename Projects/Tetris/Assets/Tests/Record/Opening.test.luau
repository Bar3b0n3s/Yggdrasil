--!strict
-- Recording suite of Opening.replay (Editor only): a planned opening of the session seed's pieces (O, J, S, Z,
-- I, T) with taps at fixed ticks: moves, both rotations, two holds, a soft drop and hard drops, ending with five
-- pieces locked in the well. It only reads the scene and injects input.
local GameScript = require("../../Scripts/Game")

type Step = { Tick: number, Action: string, State: InputEventState }

local function at(tick: number, action: string, state: InputEventState?): Step
	return { Tick = tick, Action = action, State = state or "Tap" }
end

local Plan: { Step } = {
	at(10, "MoveLeft"),
	at(14, "MoveLeft"),
	at(18, "MoveLeft"),
	at(22, "MoveLeft"),
	at(30, "HardDrop"),
	at(40, "MoveLeft"),
	at(48, "HardDrop"),
	at(56, "Hold"),
	at(64, "RotateCCW"),
	at(70, "MoveRight"),
	at(74, "MoveRight"),
	at(78, "MoveRight"),
	at(82, "MoveRight"),
	at(86, "MoveRight"),
	at(92, "SoftDrop", "Down"),
	at(152, "SoftDrop", "Up"),
	at(170, "Hold"),
	at(180, "MoveRight"),
	at(184, "MoveRight"),
	at(192, "HardDrop"),
	at(200, "RotateCW"),
	at(206, "MoveLeft"),
	at(214, "HardDrop"),
}
local EndTick = 300

return Test.Suite("Record.Opening", function()
	Test.Case("plays the planned opening", function()
		local found = Scene.FindByName("Game")
		Test.Expect(found ~= nil, "Main.scene has a Game entity")
		local game = (found :: Entity):GetScript() :: GameScript.Game
		local tick = 0
		for _, step in Plan do
			Test.WaitTicks(step.Tick - tick)
			tick = step.Tick
			Test.InjectAction(step.Action, step.State)
		end
		Test.WaitTicks(EndTick - tick)
		Test.ExpectEqual(game.Match.LockedCount, 5, "five pieces locked by the input")
		Test.ExpectEqual(game.Match.Hold, "I", "the I waits in the hold")
		Test.Expect(not game.Match.GameOver, "the game is still running")
	end, { TimeoutTicks = 600 })
end)
