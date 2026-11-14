--!strict
-- Recording suite of Restart.replay (Editor only): a planned top-out by hard drops from tick 10, then a pointer click
-- on the game-over panel's RESTART button and the new game it starts. It only reads the scene (the button's laid-out
-- rectangle included) and injects input.
local GameScript = require("../../Scripts/Game")

-- Every recording runs at the 720 x 900 window, which shows the PixelPerfectCamera's 360 x 450 scene doubled with
-- no letterbox bars: a view point is twice the scene pixel that Ui.GetRect reports.
local ViewScale = 2
local FirstDropTick = 10
local DropInterval = 3
local SettleTicks = 30

return Test.Suite("Record.Restart", function()
	Test.Case("tops out, then clicks RESTART", function()
		local found = Scene.FindByName("Game")
		Test.Expect(found ~= nil, "Main.scene has a Game entity")
		local game = (found :: Entity):GetScript() :: GameScript.Game
		Test.WaitTicks(FirstDropTick)
		local drops = 0
		while not game.Match.GameOver and drops < 40 do
			Test.InjectAction("HardDrop", "Tap")
			Test.WaitTicks(DropInterval)
			drops += 1
		end
		Test.WaitUntil(function(): boolean
			return game.Match.GameOver
		end, 10)
		Test.WaitTicks(2)
		local button = Scene.FindByName("RestartButton")
		local rect = if button then Ui.GetRect(button) else nil
		if rect == nil then
			Test.Fail("the RESTART button is not laid out at game over")
		end
		local laid = rect :: UiElementRect
		local position = (laid.Min + laid.Max) * (ViewScale / 2)
		Test.InjectMouseMove(position)
		Test.WaitTicks(1)
		Test.InjectMouse("Left", "Tap", position)
		Test.WaitTicks(SettleTicks)
		Test.Expect(not game.Match.GameOver, "the click started a new game")
		Test.ExpectEqual(game.Match.Score, 0, "the new game has no score")
		Test.ExpectEqual(game.Match.LockedCount, 0, "nothing locked in the new game")
		Test.ExpectEqual(game.Restarts, 1, "the click restarted once")
		for column = 1, 10 do
			Test.ExpectEqual(game.Match.Board:Get(column, 1), "", `the bottom row is empty at column {column}`)
		end
	end, { TimeoutTicks = 600 })
end)
