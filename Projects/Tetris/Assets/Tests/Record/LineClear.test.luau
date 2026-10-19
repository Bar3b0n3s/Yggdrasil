--!strict
-- Recording suite of LineClear.replay (Editor only): it plays the known pieces of the session seed like a player,
-- each placed where the autopilot scores best, steering with taps and a hard drop, until a line clears, and ends
-- with the cleared line on the HUD. It only reads the scene and injects input.
local Autopilot = require("../Lib/Autopilot")
local GameScript = require("../../Scripts/Game")

local function tap(action: string)
	Test.InjectAction(action, "Tap")
	Test.WaitTicks(2)
end

return Test.Suite("Record.LineClear", function()
	Test.Case("places the known pieces until a line clears", function()
		local found = Scene.FindByName("Game")
		Test.Expect(found ~= nil, "Main.scene has a Game entity")
		local game = (found :: Entity):GetScript() :: GameScript.Game
		Test.WaitTicks(10)
		local pieces = 0
		while game.Match.Lines == 0 and not game.Match.GameOver and pieces < 40 do
			local match = game.Match
			local locked = match.LockedCount
			local plan = Autopilot.Choose(match)
			if plan then
				local turns = 0
				while match.State ~= plan.State and turns < 4 do
					tap(if (plan.State - match.State) % 4 == 3 then "RotateCCW" else "RotateCW")
					turns += 1
				end
				local steps = 0
				while match.Left ~= plan.Left and steps < 12 do
					local before = match.Left
					tap(if plan.Left < match.Left then "MoveLeft" else "MoveRight")
					if match.Left == before then
						break
					end
					steps += 1
				end
			end
			tap("HardDrop")
			Test.Expect(match.LockedCount > locked, "the hard drop locked the piece")
			pieces += 1
			Test.WaitTicks(4)
		end
		Test.WaitTicks(30)
		Test.Expect(game.Match.Lines >= 1, "the placements cleared a line")
		Test.Expect(game.Match.Score >= 100, "and scored it")
		Test.Expect(not game.Match.GameOver, "the game is still running")
	end, { TimeoutTicks = 3000 })
end)
