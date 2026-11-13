--!strict
-- Integration cases in Main.scene on sprites and UI, driven through the project's input actions with Test.InjectAction
-- and, for the game-over panel's RESTART button, a pointer click with Test.InjectMouse. Each case starts from a fresh
-- scene (Isolation "Case"); a few set up the board through the match first. The cases read the well's SpriteRenderer
-- colours and Transforms, and the HUD's UiText and preview cells' UiRect, through their component proxies.
local GameScript = require("../Scripts/Game")
local Palette = require("../Scripts/Palette")
local Pieces = require("../Scripts/Pieces")

-- Every test session's view is the 720 x 900 window, which shows the PixelPerfectCamera's 360 x 450 scene doubled
-- with no letterbox bars: a view point is twice the scene pixel that Ui.GetRect reports.
local ViewScale = 2

-- Every text of the HUD: the labels and values, the clear banner, the help, the game-over panel's texts and the
-- RESTART button's label.
local HudTexts = {
	"Title",
	"HoldLabel",
	"ScoreLabel",
	"ScoreText",
	"LevelLabel",
	"LevelText",
	"LinesLabel",
	"LinesText",
	"ClearText",
	"NextLabel",
	"HelpText",
	"GameOverTitle",
	"GameOverHint",
	"RestartLabel",
}

-- Where a piece's four 16 x 16 preview cells sit inside an 80 x 48 hold or next box (each cell's top-left corner, in
-- canvas units, y down): the piece's spawn state centred on whole pixels, its bounding box at I (8, 16), O (24, 8) and
-- the 3-wide pieces (16, 8).
local PreviewCells: { [string]: { { number } } } = {
	I = { { 8, 16 }, { 24, 16 }, { 40, 16 }, { 56, 16 } },
	O = { { 24, 8 }, { 40, 8 }, { 24, 24 }, { 40, 24 } },
	T = { { 32, 8 }, { 16, 24 }, { 32, 24 }, { 48, 24 } },
	S = { { 32, 8 }, { 48, 8 }, { 16, 24 }, { 32, 24 } },
	Z = { { 16, 8 }, { 32, 8 }, { 32, 24 }, { 48, 24 } },
	J = { { 16, 8 }, { 16, 24 }, { 32, 24 }, { 48, 24 } },
	L = { { 48, 8 }, { 16, 24 }, { 32, 24 }, { 48, 24 } },
}

local function getGame(): GameScript.Game
	local found = Scene.FindByName("Game")
	if found == nil then
		Test.Fail("Main.scene has no Game entity")
	end
	return (found :: Entity):GetScript() :: GameScript.Game
end

local function find(name: string): Entity
	local found = Scene.FindByName(name)
	if found == nil then
		Test.Fail(`Main.scene has no {name}`)
	end
	return found :: Entity
end

local function tap(action: string, waitTicks: number?)
	Test.InjectAction(action, "Tap")
	Test.WaitTicks(waitTicks or 2)
end

-- The text a HUD element's UiText shows.
local function textOf(name: string): string
	local text = find(name).UiText
	return if text then text.Text else "<missing>"
end

-- The colour a well cell's SpriteRenderer shows (column 1 to 10, row 1 at the bottom to 20).
local function cellColor(game: GameScript.Game, column: number, row: number): Color
	local renderer = game.Cells[(row - 1) * 10 + column].SpriteRenderer
	return if renderer then renderer.Color else Color.New(0, 0, 0, 0)
end

-- Expects every well cell on its square: game.Cells[(row - 1) * 10 + column] has its Transform translation at
-- (column - 5.5, row - 10.5, 0), row 1 at the bottom and column 1 at the left, so a well drawn upside down, mirrored
-- or shifted fails. The comparison is exact: every coordinate is a binary fraction.
local function expectWellInPlace(game: GameScript.Game)
	for row = 1, 20 do
		for column = 1, 10 do
			local cell = game.Cells[(row - 1) * 10 + column]
			local x, y = column - 5.5, row - 10.5
			local placed = "<no Transform>"
			local exact = false
			local transform = cell.Transform
			if transform then
				local at = transform.Translation
				placed = `{at.x},{at.y},{at.z}`
				exact = at.x == x and at.y == y and at.z == 0
			end
			Test.Expect(exact, `{cell.Name} (column {column}, row {row}) sits at {x},{y},0 (placed at {placed})`)
		end
	end
end

-- Counts the cells of a row that show a block (neither the empty nor the ghost colour).
local function countShown(game: GameScript.Game, row: number): number
	local count = 0
	for column = 1, 10 do
		local color = cellColor(game, column, row)
		if color ~= Palette.Empty and color ~= Palette.Ghost then
			count += 1
		end
	end
	return count
end

-- Counts the cells of the whole well that show a colour.
local function countColor(game: GameScript.Game, color: Color): number
	local count = 0
	for row = 1, 20 do
		for column = 1, 10 do
			if cellColor(game, column, row) == color then
				count += 1
			end
		end
	end
	return count
end

-- Expects the ghost colour on every block of the falling piece's landing position.
local function expectGhostBelowPiece(game: GameScript.Game)
	local match = game.Match
	local ghostTop = match.Top - match:DropDistance()
	for _, block in Pieces.Get(match.Piece).States[match.State + 1] do
		local column, row = match.Left + block.X, ghostTop - block.Y
		Test.Expect(cellColor(game, column, row) == Palette.Ghost, `a ghost block at column {column}, row {row}`)
	end
end

-- Expects preview slot `slot` (0 the hold, 1 to 3 the NEXT boxes) to show `piece` in its box `boxName` (HoldBox or
-- NextBox<slot>): the slot's four preview cells (Preview<slot + 1>_1 to _4, game.Previews[slot * 4 + 1] to
-- [slot * 4 + 4]) are active children of the box, each in the piece's colour, together on the piece's four cell
-- positions in the box, both as placed (UiRect.Position) and as laid out (Ui.GetRect, relative to the box), and
-- 16 x 16.
local function expectPreview(game: GameScript.Game, slot: number, boxName: string, piece: string)
	local box = find(boxName)
	local boxRect = Ui.GetRect(box)
	if boxRect == nil then
		Test.Fail(`{boxName} is not laid out`)
	end
	local boxMin = (boxRect :: UiElementRect).Min
	local open: { [string]: boolean } = {}
	for _, position in PreviewCells[piece] do
		open[`{position[1]},{position[2]}`] = true
	end
	for i = 1, 4 do
		local cell = game.Previews[slot * 4 + i]
		local label = `{boxName} cell {i} ({cell.Name})`
		Test.Expect(cell:IsActive(), `{label} shows`)
		local parent = cell:GetParent()
		Test.Expect(parent ~= nil and parent == box, `{label} sits in {boxName}`)
		local image = cell.UiImage
		Test.Expect(image ~= nil and image.Color == Palette.Pieces[piece], `{label} shows the {piece}'s colour`)
		local rect = cell.UiRect
		local placed = if rect then `{rect.Position.x},{rect.Position.y}` else "<no UiRect>"
		Test.Expect(open[placed] == true, `{label} sits on a free cell of the {piece} (placed at {placed})`)
		open[placed] = nil
		local laid = Ui.GetRect(cell)
		if laid == nil then
			Test.Fail(`{label} is not laid out`)
		end
		local cellRect = laid :: UiElementRect
		local offset = cellRect.Min - boxMin
		local size = cellRect.Max - cellRect.Min
		Test.ExpectEqual(`{offset.x},{offset.y}`, placed, `{label} is laid out where it is placed`)
		Test.ExpectEqual(`{size.x},{size.y}`, "16,16", `{label} is 16 x 16`)
	end
end

-- Taps HardDrop until the stack tops out, at most 40 times.
local function topOut(game: GameScript.Game)
	local drops = 0
	while not game.Match.GameOver and drops < 40 do
		tap("HardDrop", 3)
		drops += 1
	end
	Test.Expect(game.Match.GameOver, `the stack topped out after {drops} drops`)
end

-- The view position of an element's centre: its laid-out rectangle's centre in scene pixels, doubled.
local function viewCentre(name: string): vector
	local rect = Ui.GetRect(find(name))
	if rect == nil then
		Test.Fail(`{name} is not laid out`)
	end
	local laid = rect :: UiElementRect
	return (laid.Min + laid.Max) * (ViewScale / 2)
end

-- Expects the new game after a restart: running, no score, nothing locked, an empty bottom row and no panel.
local function expectFreshGame(game: GameScript.Game)
	Test.Expect(not game.Match.GameOver, "a new game started")
	Test.ExpectEqual(game.Match.Score, 0)
	Test.ExpectEqual(game.Match.LockedCount, 0)
	Test.ExpectEqual(textOf("ScoreText"), "0")
	Test.Expect(not find("GameOverPanel"):IsActiveSelf(), "the game-over panel is hidden")
	Test.ExpectEqual(countShown(game, 1), 0, "the well was emptied")
end

return Test.Suite("Tetris", function()
	Test.Case("the scene pools the well, the previews and the HUD", function()
		Test.WaitTicks(1)
		local game = getGame()
		Test.ExpectEqual(#game.Cells, 200, "one cell per visible square")
		local sprites, meshes, active = 0, 0, 0
		for _, cell in game.Cells do
			if cell.SpriteRenderer ~= nil then
				sprites += 1
			end
			if cell.MeshRenderer ~= nil then
				meshes += 1
			end
			if cell:IsActive() then
				active += 1
			end
		end
		Test.ExpectEqual(sprites, 200, "every cell is a sprite")
		Test.ExpectEqual(meshes, 0, "no cell has a MeshRenderer")
		Test.ExpectEqual(active, 200, "every cell is active")
		expectWellInPlace(game)
		local empty = countColor(game, Palette.Empty)
		Test.Expect(empty >= 192 and empty <= 196, `only the falling piece and its ghost leave the empty colour (got {empty})`)
		Test.ExpectEqual(countColor(game, Palette.Ghost), 4, "four ghost cells")
		Test.ExpectEqual(#game.Previews, 16, "four cells per preview slot")
		for _, cell in game.Previews do
			Test.Expect(cell.UiImage ~= nil, `{cell.Name} is a UI image`)
		end
		for _, name in HudTexts do
			local entity = find(name)
			Test.Expect(entity.UiText ~= nil, `{name} is a UiText`)
			Test.Expect(entity.Text == nil, `{name} has no Text component`)
		end
		Test.ExpectEqual(textOf("ScoreText"), "0")
		Test.ExpectEqual(textOf("LevelText"), "1")
		Test.ExpectEqual(textOf("LinesText"), "0")
		Test.ExpectEqual(textOf("ClearText"), "")
		Test.Expect(not find("GameOverPanel"):IsActiveSelf(), "the game-over panel starts hidden")
		Test.ExpectEqual(#game.Match.Queue, 3, "three next pieces")
		Test.Expect(game.Previews[5]:IsActiveSelf(), "the first next slot shows a piece")
		Test.Expect(not game.Previews[1]:IsActiveSelf(), "the hold slot starts empty")
		local camera = find("Camera").PixelPerfectCamera
		Test.Expect(camera ~= nil, "the camera is pixel-perfect")
		if camera then
			Test.ExpectEqual(camera.ReferenceWidth, 360)
			Test.ExpectEqual(camera.ReferenceHeight, 450)
			Test.ExpectEqual(camera.PixelsPerUnit, 16)
		end
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
				Test.Expect(cellColor(game, column, 1) == Palette.Pieces[piece], `column {column} shows the {piece}'s colour`)
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
		-- Simulation.Seed 1717's first bag starts with the O, so the first hold puts the known O in the hold box.
		Test.ExpectEqual(first, "O", "the first bag starts with the O")
		tap("Hold")
		Test.ExpectEqual(game.Match.Hold, first, "the piece went to the hold")
		Test.ExpectEqual(game.Match.Piece, second, "the next piece came in")
		expectPreview(game, 0, "HoldBox", first)
		tap("Hold")
		Test.ExpectEqual(game.Match.Piece, second, "a second hold of the same piece does nothing")
		tap("HardDrop")
		tap("Hold")
		Test.ExpectEqual(game.Match.Piece, first, "the next piece swaps with the held one")
	end)

	Test.Case("the next boxes show the queued pieces centred in their boxes", function()
		Test.WaitTicks(2)
		local game = getGame()
		-- Simulation.Seed 1717's first bag is O, J, S, Z, I, T, L: the O falls and J, S and Z are queued.
		Test.ExpectEqual(game.Match.Piece, "O", "the first bag starts with the O")
		Test.ExpectEqual(table.concat(game.Match.Queue, ","), "J,S,Z", "the bag queues J, S and Z")
		local start = { "J", "S", "Z" }
		for index = 1, 3 do
			expectPreview(game, index, `NextBox{index}`, start[index])
		end
		tap("HardDrop")
		Test.ExpectEqual(game.Match.Piece, "J", "the first queued piece came in")
		Test.ExpectEqual(table.concat(game.Match.Queue, ","), "S,Z,I", "the queue moved up and the I joined it")
		local after = { "S", "Z", "I" }
		for index = 1, 3 do
			expectPreview(game, index, `NextBox{index}`, after[index])
		end
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
		Test.ExpectEqual(countColor(game, Palette.Ghost), 4, "the well shows four ghost cells")
		expectGhostBelowPiece(game)
		local left = game.Match.Left
		tap("MoveLeft")
		Test.ExpectEqual(game.Match.Left, left - 1, "the piece moved one column left")
		Test.ExpectEqual(countColor(game, Palette.Ghost), 4, "still four ghost cells after the move")
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
		Test.ExpectEqual(textOf("ScoreText"), "100")
		Test.ExpectEqual(textOf("LinesText"), "1")
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
		Test.ExpectEqual(textOf("LevelText"), "2")
		Test.ExpectEqual(textOf("LinesText"), "10")
	end)

	Test.Case("stacking hard drops tops out, then Restart starts a new game", function()
		Test.WaitTicks(1)
		local game = getGame()
		topOut(game)
		Test.Expect(find("GameOverPanel"):IsActiveSelf(), "the game-over panel shows")
		Test.ExpectEqual(textOf("GameOverTitle"), "GAME OVER")
		local locked = game.Match.LockedCount
		tap("HardDrop")
		tap("MoveLeft")
		Test.WaitTicks(120)
		Test.ExpectEqual(game.Match.LockedCount, locked, "the game waits after game over")
		Test.ExpectEqual(#Test.GetScriptErrors(), 0, "no script error at top-out")
		tap("Restart")
		expectFreshGame(game)
	end, { TimeoutTicks = 600 })

	Test.Case("game over shows the panel and its RESTART button restarts by a pointer click", function()
		Test.WaitTicks(1)
		local game = getGame()
		topOut(game)
		Test.WaitTicks(2)
		Test.Expect(find("GameOverPanel"):IsActiveSelf(), "the game-over panel shows")
		local restarts = game.Restarts
		local position = viewCentre("RestartButton")
		Test.InjectMouseMove(position)
		Test.WaitTicks(1)
		Test.InjectMouse("Left", "Tap", position)
		Test.WaitTicks(3)
		Test.ExpectEqual(game.Restarts, restarts + 1, "the click restarted once")
		expectFreshGame(game)
	end, { TimeoutTicks = 600 })

	Test.Case("a Restart action and a click in one tick restart once", function()
		Test.WaitTicks(1)
		local game = getGame()
		topOut(game)
		Test.WaitTicks(2)
		local restarts = game.Restarts
		local position = viewCentre("RestartButton")
		Test.InjectMouseMove(position)
		Test.InjectMouse("Left", "Down", position)
		Test.WaitTicks(1)
		Test.ExpectEqual(game.Restarts, restarts, "a press alone does not click")
		Test.InjectMouse("Left", "Up", position)
		Test.InjectAction("Restart", "Tap")
		Test.WaitTicks(1)
		Test.ExpectEqual(game.Restarts, restarts + 1, "the click and the action restarted in the same tick")
		Test.WaitTicks(3)
		Test.ExpectEqual(game.Restarts, restarts + 1, "and only once")
		Test.Expect(not game.RestartClicked, "no click is left pending")
		expectFreshGame(game)
	end, { TimeoutTicks = 600 })
end)
