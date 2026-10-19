--!strict
-- Integration cases in Main.scene, driven through the project's input actions with Test.InjectAction.
-- Each case starts from a fresh scene (Isolation "Case"); a few set up the board through the match first.
local GameScript = require("../Scripts/Game")
local Pieces = require("../Scripts/Pieces")

local function getGame(): GameScript.Game
	local found = Scene.FindByName("Game")
	if found == nil then
		Test.Fail("Main.scene has no Game entity")
	end
	return (found :: Entity):GetScript() :: GameScript.Game
end

local function tap(action: string, waitTicks: number?)
	Test.InjectAction(action, "Tap")
	Test.WaitTicks(waitTicks or 2)
end

local function textOf(name: string): string
	local found = Scene.FindByName(name)
	local text = if found then found.Text else nil
	return if text then text.Text else "<missing>"
end

-- Counts the visible cells of the well that show a block of a piece (not the ghost).
local function countShown(game: GameScript.Game, row: number): number
	local count = 0
	for column = 1, 10 do
		local name = game.ShownName[(row - 1) * 10 + column]
		if name ~= "" and name ~= "Ghost" then
			count += 1
		end
	end
	return count
end

-- Counts the cells of the whole well that show the ghost piece.
local function countGhosts(game: GameScript.Game): number
	local count = 0
	for _, name in game.ShownName do
		if name == "Ghost" then
			count += 1
		end
	end
	return count
end

-- Expects every block of the ghost at the falling piece's columns in its landing row.
local function expectGhostBelowPiece(game: GameScript.Game)
	local match = game.Match
	local ghostTop = match.Top - match:DropDistance()
	for _, block in Pieces.Get(match.Piece).States[match.State + 1] do
		local column, row = match.Left + block.X, ghostTop - block.Y
		Test.ExpectEqual(game.ShownName[(row - 1) * 10 + column], "Ghost", `a ghost block at column {column}, row {row}`)
	end
end

return Test.Suite("Tetris", function()
	Test.Case("the scene pools the well, the previews and the HUD", function()
		Test.WaitTicks(1)
		local game = getGame()
		Test.ExpectEqual(#game.Cells, 200, "one cell per visible square")
		Test.ExpectEqual(#game.Previews, 16, "four cells per preview slot")
		local active = 0
		for _, cell in game.Cells do
			if cell:IsActiveSelf() then
				active += 1
			end
		end
		Test.Expect(active >= 2 and active <= 8, `only the falling piece and its ghost show at the start (got {active})`)
		Test.ExpectEqual(textOf("ScoreText"), "SCORE\n0")
		Test.ExpectEqual(textOf("LevelText"), "LEVEL\n1")
		Test.ExpectEqual(textOf("LinesText"), "LINES\n0")
		Test.ExpectEqual(textOf("StatusText"), "")
		Test.ExpectEqual(#game.Match.Queue, 3, "three next pieces")
		Test.Expect(game.Previews[5]:IsActiveSelf(), "the first next slot shows a piece")
		Test.Expect(not game.Previews[1]:IsActiveSelf(), "the hold slot starts empty")
	end)

	Test.Case("a hard drop locks the piece on the floor", function()
		Test.WaitTicks(1)
		local game = getGame()
		local piece = game.Match.Piece
		tap("HardDrop")
		Test.ExpectEqual(game.Match.LockedCount, 1, "one piece locked")
		Test.Expect(countShown(game, 1) >= 1, "blocks show in the bottom row")
		Test.Expect(game.Match.Piece ~= "", "the next piece spawned")
		local found = false
		for column = 1, 10 do
			if game.Match.Board:Get(column, 1) == piece then
				found = true
			end
		end
		Test.Expect(found, `the {piece} lies in row 1`)
	end)

	Test.Case("taps move one column and a held move auto-repeats to the wall", function()
		Test.WaitTicks(1)
		local game = getGame()
		local left = game.Match.Left
		tap("MoveLeft")
		Test.ExpectEqual(game.Match.Left, left - 1, "a tap moves one column left")
		tap("MoveRight")
		Test.ExpectEqual(game.Match.Left, left, "a tap moves one column right")
		Test.InjectAction("MoveRight", "Down")
		Test.WaitTicks(30)
		Test.InjectAction("MoveRight", "Up")
		Test.WaitTicks(1)
		Test.Expect(not game.Match:Move(1), "DAS carried the piece to the right wall")
	end)

	Test.Case("rotate actions turn the piece both ways", function()
		Test.WaitTicks(1)
		local game = getGame()
		if game.Match.Piece == "O" then
			tap("HardDrop")
		end
		Test.ExpectEqual(game.Match.State, 0)
		tap("RotateCW")
		Test.ExpectEqual(game.Match.State, 1, "clockwise")
		tap("RotateCW")
		Test.ExpectEqual(game.Match.State, 2, "clockwise again")
		tap("RotateCCW")
		Test.ExpectEqual(game.Match.State, 1, "counter-clockwise")
	end)

	Test.Case("hold stores the piece once per piece and shows it", function()
		Test.WaitTicks(1)
		local game = getGame()
		local first = game.Match.Piece
		local second = game.Match.Queue[1]
		tap("Hold")
		Test.ExpectEqual(game.Match.Hold, first, "the piece went to the hold")
		Test.ExpectEqual(game.Match.Piece, second, "the next piece came in")
		Test.Expect(game.Previews[1]:IsActiveSelf(), "the hold preview shows it")
		tap("Hold")
		Test.ExpectEqual(game.Match.Piece, second, "a second hold of the same piece does nothing")
		tap("HardDrop")
		tap("Hold")
		Test.ExpectEqual(game.Match.Piece, first, "the next piece swaps with the held one")
	end)

	Test.Case("soft drop falls faster than gravity", function()
		Test.WaitTicks(1)
		local game = getGame()
		local top = game.Match.Top
		Test.InjectAction("SoftDrop", "Down")
		Test.WaitTicks(12)
		Test.InjectAction("SoftDrop", "Up")
		Test.WaitTicks(1)
		Test.Expect(game.Match.Top <= top - 5, `soft drop fell 5 rows or more (from {top} to {game.Match.Top})`)
	end)

	Test.Case("the ghost shows where the piece will land", function()
		Test.WaitTicks(1)
		local game = getGame()
		Test.ExpectEqual(countGhosts(game), 4, "the well shows four ghost cells")
		expectGhostBelowPiece(game)
		local left = game.Match.Left
		tap("MoveLeft")
		Test.ExpectEqual(game.Match.Left, left - 1, "the piece moved one column left")
		Test.ExpectEqual(countGhosts(game), 4, "still four ghost cells after the move")
		expectGhostBelowPiece(game)
	end)

	Test.Case("clearing a line scores and shows it on the HUD", function()
		Test.WaitTicks(1)
		local game = getGame()
		game.Match.Board:SetRow(1, "XXXX..XXXX")
		game.Match:SpawnPiece("O")
		tap("HardDrop")
		Test.ExpectEqual(game.Match.Lines, 1, "one line cleared")
		Test.ExpectEqual(game.Match.Score, 100)
		Test.ExpectEqual(textOf("ScoreText"), "SCORE\n100")
		Test.ExpectEqual(textOf("LinesText"), "LINES\n1")
		Test.ExpectEqual(textOf("ClearText"), "SINGLE\n+100")
		Test.ExpectEqual(countShown(game, 1), 2, "the O's top half dropped into row 1")
		Test.WaitTicks(130)
		Test.ExpectEqual(textOf("ClearText"), "", "the clear banner fades")
	end)

	Test.Case("four lines with a vertical I score a tetris", function()
		Test.WaitTicks(1)
		local game = getGame()
		for row = 1, 4 do
			game.Match.Board:SetRow(row, "XXXXXXXXX.")
		end
		game.Match:SpawnPiece("I")
		tap("RotateCW")
		for _ = 1, 4 do
			tap("MoveRight")
		end
		tap("HardDrop")
		Test.ExpectEqual(game.Match.Lines, 4, "four lines cleared")
		Test.ExpectEqual(game.Match.Score, 800)
		Test.ExpectEqual(textOf("ClearText"), "TETRIS!\n+800")
		Test.ExpectEqual(countShown(game, 1), 0, "the well is empty again")
	end)

	Test.Case("ten lines raise the level", function()
		Test.WaitTicks(1)
		local game = getGame()
		game.Match.Lines = 9
		game.Match.Board:SetRow(1, "XXXX..XXXX")
		game.Match:SpawnPiece("O")
		tap("HardDrop")
		Test.ExpectEqual(game.Match.Level, 2)
		Test.ExpectEqual(textOf("LevelText"), "LEVEL\n2")
		Test.ExpectEqual(textOf("LinesText"), "LINES\n10")
	end)

	Test.Case("stacking hard drops tops out, then Restart starts a new game", function()
		Test.WaitTicks(1)
		local game = getGame()
		local drops = 0
		while not game.Match.GameOver and drops < 40 do
			tap("HardDrop", 3)
			drops += 1
		end
		Test.Expect(game.Match.GameOver, `the stack topped out after {drops} drops`)
		Test.ExpectEqual(textOf("StatusText"), "GAME OVER\nPress Enter to restart")
		local locked = game.Match.LockedCount
		tap("HardDrop")
		tap("MoveLeft")
		Test.WaitTicks(120)
		Test.ExpectEqual(game.Match.LockedCount, locked, "the game waits after game over")
		Test.ExpectEqual(#Test.GetScriptErrors(), 0, "no script error at top-out")
		tap("Restart")
		Test.Expect(not game.Match.GameOver, "a new game started")
		Test.ExpectEqual(game.Match.Score, 0)
		Test.ExpectEqual(game.Match.LockedCount, 0)
		Test.ExpectEqual(textOf("StatusText"), "")
		Test.ExpectEqual(countShown(game, 1), 0, "the well was emptied")
	end, { TimeoutTicks = 600 })
end)
