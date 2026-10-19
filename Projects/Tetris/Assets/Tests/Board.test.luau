--!strict
-- Pure-logic cases of the Tetris rules (no scene): SRS kicks, line clears and scores, the 7-bag, top-out,
-- hold, gravity, soft drop, lock delay, DAS/ARR, levels and restart.
local Bag = require("../Scripts/Bag")
local Match = require("../Scripts/Match")
local Pieces = require("../Scripts/Pieces")

type Controls = Match.Controls

local function newMatch(seed: number): Match.Match
	return Match.New(Random.New(seed))
end

-- Steps the match `ticks` times with the same controls.
local function stepTimes(match: Match.Match, controls: Controls, ticks: number)
	for _ = 1, ticks do
		match:Step(controls)
	end
end

local function hasEvent(match: Match.Match, event: string): boolean
	return table.find(match.Events, event) ~= nil
end

-- Expects that the last lock ended the game by itself: the locked piece is still the match's piece and no next
-- piece left the queue (a spawn after the lock takes one, whether or not it then fits, so a game over that a
-- blocked spawn caused fails here).
local function expectNoSpawn(match: Match.Match, piece: string, left: number, queue: { string })
	Test.ExpectEqual(match.Piece, piece, "no next piece spawned")
	Test.ExpectEqual(match.Left, left, "the locked piece was not replaced by a spawn")
	Test.ExpectEqual(#match.Queue, #queue, "the queue keeps its length")
	for i, name in queue do
		Test.ExpectEqual(match.Queue[i], name, `next piece {i} was not taken`)
	end
end

-- Fills `lines` bottom rows except column 10, drops a vertical I into the gap and returns the match.
local function clearWithI(lines: number, level: number): Match.Match
	local match = newMatch(5)
	match.Level = level
	match.Lines = (level - 1) * Match.LinesPerLevel
	for row = 1, lines do
		match.Board:SetRow(row, "XXXXXXXXX.")
	end
	match:SpawnPiece("I")
	match.State = 1
	match.Left = 8
	match.Top = 20
	Test.Expect(match.Board:Fits("I", 1, 8, 20), "the vertical I fits above the gap")
	match:HardDrop()
	return match
end

return Test.Suite("Board", function()
	Test.Case("T kicks off the left wall", function()
		local match = newMatch(1)
		match:SpawnPiece("T")
		match.State, match.Left, match.Top = 1, 0, 10
		Test.Expect(match.Board:Fits("T", 1, 0, 10), "T in state R touches the left wall")
		Test.Expect(not match.Board:Fits("T", 2, 0, 10), "the unkicked rotation overlaps the wall")
		Test.Expect(match:Rotate(1), "the rotation succeeds through a kick")
		Test.ExpectEqual(match.State, 2)
		Test.ExpectEqual(match.Left, 1, "kicked one column right")
		Test.ExpectEqual(match.Top, 10)
	end)

	Test.Case("T kicks off the right wall", function()
		local match = newMatch(1)
		match:SpawnPiece("T")
		match.State, match.Left, match.Top = 3, 9, 10
		Test.Expect(match.Board:Fits("T", 3, 9, 10), "T in state L touches the right wall")
		Test.Expect(match:Rotate(1), "the rotation succeeds through a kick")
		Test.ExpectEqual(match.State, 0)
		Test.ExpectEqual(match.Left, 8, "kicked one column left")
		Test.ExpectEqual(match.Top, 10)
	end)

	Test.Case("I kicks off both walls", function()
		local left = newMatch(1)
		left:SpawnPiece("I")
		left.State, left.Left, left.Top = 1, -1, 10
		Test.Expect(left.Board:Fits("I", 1, -1, 10), "vertical I in column 1")
		Test.Expect(left:Rotate(-1), "R to 0 succeeds through a kick")
		Test.ExpectEqual(left.State, 0)
		Test.ExpectEqual(left.Left, 1, "kicked two columns right")

		local right = newMatch(1)
		right:SpawnPiece("I")
		right.State, right.Left, right.Top = 3, 9, 10
		Test.Expect(right.Board:Fits("I", 3, 9, 10), "vertical I in column 10")
		Test.Expect(right:Rotate(1), "L to 0 succeeds through a kick")
		Test.ExpectEqual(right.State, 0)
		Test.ExpectEqual(right.Left, 7, "kicked two columns left")
	end)

	Test.Case("T and I kick up off the floor", function()
		local t = newMatch(1)
		t:SpawnPiece("T")
		t.State, t.Left, t.Top = 0, 4, 2
		Test.ExpectEqual(t:DropDistance(), 0, "T rests on the floor")
		Test.Expect(t:Rotate(1), "0 to R succeeds through a kick")
		Test.ExpectEqual(t.State, 1)
		Test.ExpectEqual(t.Left, 3)
		Test.ExpectEqual(t.Top, 3, "kicked one row up")

		local i = newMatch(1)
		i:SpawnPiece("I")
		i.State, i.Left, i.Top = 0, 4, 2
		Test.ExpectEqual(i:DropDistance(), 0, "I rests on the floor")
		Test.Expect(i:Rotate(1), "0 to R succeeds through a kick")
		Test.ExpectEqual(i.Left, 5)
		Test.ExpectEqual(i.Top, 4, "kicked two rows up")
	end)

	Test.Case("a blocked rotation fails and leaves the piece", function()
		local match = newMatch(1)
		for row = 1, 22 do
			match.Board:SetRow(row, "XXX...XXXX")
		end
		match:SpawnPiece("I")
		match.State, match.Left, match.Top = 1, 2, 12
		Test.Expect(match.Board:Fits("I", 1, 2, 12), "vertical I in the shaft")
		Test.Expect(not match:Rotate(1), "no kick fits in a three-wide shaft")
		Test.ExpectEqual(match.State, 1)
		Test.ExpectEqual(match.Left, 2)
	end)

	Test.Case("clears of 1 to 4 lines score 100, 300, 500 and 800 times the level", function()
		local expected = { 100, 300, 500, 800 }
		for lines = 1, 4 do
			local match = clearWithI(lines, 1)
			Test.ExpectEqual(match.Lines, lines, `{lines} lines cleared`)
			Test.ExpectEqual(match.Score, expected[lines], `score of {lines} lines`)
			Test.ExpectEqual(match.LastClear, lines)
			Test.Expect(hasEvent(match, `Clear{lines}`), "a clear event")
			Test.ExpectEqual(match.Board:Get(10, 1), if lines < 4 then "I" else "", "the rest of the I fell into the cleared rows")
			Test.ExpectEqual(match.Board:Get(1, 1), "", "the filled rows are gone")
		end
		local levelThree = clearWithI(4, 3)
		Test.ExpectEqual(levelThree.Score, 2400, "a tetris at level 3")
		local levelTwo = clearWithI(2, 2)
		Test.ExpectEqual(levelTwo.Score, 600, "a double at level 2")
	end)

	Test.Case("levels rise every 10 lines", function()
		local match = newMatch(5)
		match.Lines = 8
		match.Board:SetRow(1, "XXXXXXXXX.")
		match.Board:SetRow(2, "XXXXXXXXX.")
		match:SpawnPiece("I")
		match.State, match.Left, match.Top = 1, 8, 20
		match:HardDrop()
		Test.ExpectEqual(match.Lines, 10)
		Test.ExpectEqual(match.Level, 2)
		Test.ExpectEqual(match.Score, 300, "scored at the level before the rise")
		Test.Expect(hasEvent(match, "LevelUp"), "a level-up event")
		Test.ExpectEqual(match:GravityTicks(), 43, "gravity of level 2")
	end)

	Test.Case("the 7-bag yields each piece once per bag", function()
		local bag = Bag.New(Random.New(1234))
		for round = 1, 4 do
			local seen: { [string]: boolean } = {}
			for _ = 1, 7 do
				local name = bag:Draw()
				Test.Expect(not seen[name], `bag {round} repeats {name}`)
				seen[name] = true
			end
			for _, name in Pieces.Names do
				Test.Expect(seen[name] == true, `bag {round} lacks {name}`)
			end
		end
	end)

	Test.Case("the match deals its first seven pieces from one bag", function()
		local match = newMatch(77)
		local seen: { [string]: boolean } = {}
		for _ = 1, 7 do
			Test.Expect(not seen[match.Piece], `{match.Piece} came twice`)
			seen[match.Piece] = true
			match:HardDrop()
		end
		Test.ExpectEqual(#match.Queue, Match.PreviewCount)
	end)

	-- Lock out only: the O locks in rows 21 and 22 while the spawn area stays free, so neither a blocked spawn nor
	-- the seed's next piece can end the game; only Place's top-out result can.
	Test.Case("a lock above the visible rows tops out and ends the game", function()
		local match = newMatch(3)
		for row = 1, 20 do
			match.Board:SetRow(row, ".XXXXXXXXX")
		end
		Test.Expect(match:SpawnPiece("O"), "the O spawns")
		Test.ExpectEqual(match.Top, 22, "row 20 is full under the O, so it stays in rows 21 and 22")
		match.Left = 9
		Test.Expect(match.Board:Fits("O", 0, 9, 22), "the O fits in columns 9 and 10")
		local queue = table.clone(match.Queue)
		Test.ExpectEqual(match:HardDrop(), 0, "the O cannot fall")
		Test.Expect(match.GameOver, "the lock out ended the game")
		Test.Expect(hasEvent(match, "GameOver"), "a game-over event")
		for _, cell in { { 9, 21 }, { 10, 21 }, { 9, 22 }, { 10, 22 } } do
			Test.ExpectEqual(match.Board:Get(cell[1], cell[2]), "O", `the O's block at column {cell[1]}, row {cell[2]}`)
		end
		expectNoSpawn(match, "O", 9, queue)
		local locked = match.LockedCount
		local controls = Match.NewControls()
		controls.HardDrop = true
		match:Step(controls)
		Test.ExpectEqual(match.LockedCount, locked, "nothing moves after game over")
		Test.Expect(not match:HoldPiece(), "no hold after game over")
	end)

	-- The (+1, +2) kick of 0 to R lifts the I's top block into row 23, above the well; the other three blocks lock
	-- in rows 20 to 22. The lock itself must end the game (expectNoSpawn), not the next spawn that those blocks
	-- would block.
	Test.Case("an I kicked above the well tops out when it locks", function()
		local match = newMatch(3)
		match.Board:SetRow(19, "...X.XX...")
		Test.Expect(match:SpawnPiece("I"), "the I spawns")
		Test.ExpectEqual(match.Top, 21, "the I dropped one row, into row 20")
		Test.Expect(match:Rotate(1), "the rotation succeeds through a kick")
		Test.ExpectEqual(match.State, 1)
		Test.ExpectEqual(match.Left, 5, "kicked one column right")
		Test.ExpectEqual(match.Top, 23, "and two rows up: the I's top block lies above the well")
		local queue = table.clone(match.Queue)
		Test.ExpectEqual(match:HardDrop(), 0, "row 19 holds the I where it is")
		Test.Expect(match.GameOver, "the block above the well ended the game")
		Test.Expect(hasEvent(match, "GameOver"), "a game-over event")
		for row = 20, 22 do
			Test.ExpectEqual(match.Board:Get(7, row), "I", `the I's block in row {row}`)
		end
		expectNoSpawn(match, "I", 5, queue)
	end)

	Test.Case("a blocked spawn tops out and ends the game", function()
		local match = newMatch(3)
		match.Board:SetRow(21, "...XXXX...")
		match.Board:SetRow(22, "...XXXX...")
		Test.Expect(not match:SpawnPiece("T"), "the spawn is blocked")
		Test.Expect(match.GameOver, "game over")
	end)

	Test.Case("hold swaps once per piece", function()
		local match = newMatch(21)
		local first = match.Piece
		local second = match.Queue[1]
		Test.Expect(match:HoldPiece(), "the first hold succeeds")
		Test.ExpectEqual(match.Hold, first)
		Test.ExpectEqual(match.Piece, second, "the next piece comes in when nothing was held")
		Test.Expect(not match:HoldPiece(), "a second hold of the same piece is refused")
		Test.ExpectEqual(match.Hold, first)
		Test.ExpectEqual(match.Piece, second)
		match:HardDrop()
		local third = match.Piece
		Test.Expect(match:HoldPiece(), "the next piece may hold again")
		Test.ExpectEqual(match.Piece, first, "the held piece comes back")
		Test.ExpectEqual(match.Hold, third)
		Test.ExpectEqual(match.State, 0, "the swapped-in piece starts at its spawn state")
	end)

	Test.Case("gravity drops one row per level interval and soft drop is faster", function()
		local match = newMatch(9)
		local idle = Match.NewControls()
		local top = match.Top
		stepTimes(match, idle, 47)
		Test.ExpectEqual(match.Top, top, "no fall before 48 ticks at level 1")
		stepTimes(match, idle, 1)
		Test.ExpectEqual(match.Top, top - 1, "one row after 48 ticks")

		local fast = newMatch(9)
		fast.Level = 10
		local fastTop = fast.Top
		stepTimes(fast, idle, 6)
		Test.ExpectEqual(fast.Top, fastTop - 1, "one row after 6 ticks at level 10")

		local soft = newMatch(9)
		local soften = Match.NewControls()
		soften.SoftDrop = true
		local softTop = soft.Top
		stepTimes(soft, soften, 10)
		Test.ExpectEqual(soft.Top, softTop - 5, "soft drop falls a row every 2 ticks")
	end)

	Test.Case("a grounded piece locks after the lock delay", function()
		local match = newMatch(9)
		match:SpawnPiece("O")
		match.Top = 2
		local idle = Match.NewControls()
		stepTimes(match, idle, Match.LockDelay - 1)
		Test.ExpectEqual(match.LockedCount, 0, "not locked before the delay")
		stepTimes(match, idle, 1)
		Test.ExpectEqual(match.LockedCount, 1, "locked at the delay")
		Test.ExpectEqual(match.Board:Get(5, 1), "O")
		Test.ExpectEqual(match.Board:Get(6, 2), "O")
	end)

	Test.Case("moving a grounded piece resets its lock delay", function()
		local match = newMatch(9)
		match:SpawnPiece("O")
		match.Top = 2
		local idle = Match.NewControls()
		stepTimes(match, idle, 20)
		Test.Expect(match:Move(-1), "the grounded piece moves")
		stepTimes(match, idle, 20)
		Test.ExpectEqual(match.LockedCount, 0, "the move gave the lock delay back")
		stepTimes(match, idle, 10)
		Test.ExpectEqual(match.LockedCount, 1, "then it locks")
	end)

	Test.Case("DAS repeats a held move after 10 ticks, then every 2 ticks", function()
		local match = newMatch(9)
		match:SpawnPiece("T")
		local controls = Match.NewControls()
		controls.Left = true
		controls.LeftPressed = true
		match:Step(controls)
		Test.ExpectEqual(match.Left, 3, "the press moves at once")
		controls.LeftPressed = false
		stepTimes(match, controls, 9)
		Test.ExpectEqual(match.Left, 3, "no repeat during the delay")
		stepTimes(match, controls, 1)
		Test.ExpectEqual(match.Left, 2, "the first repeat after 10 ticks")
		stepTimes(match, controls, 2)
		Test.ExpectEqual(match.Left, 1, "then every 2 ticks")
		stepTimes(match, controls, 6)
		Test.ExpectEqual(match.Left, 1, "stopped by the wall")
		controls.Left = false
		match:Step(controls)
		Test.ExpectEqual(match.ShiftDirection, 0, "release stops the shift")
	end)

	Test.Case("a hard drop locks at once at the ghost position", function()
		local match = newMatch(9)
		match:SpawnPiece("O")
		local fall = match:DropDistance()
		Test.ExpectEqual(match.Top - fall, 2, "the ghost of an O on an empty well")
		Test.ExpectEqual(match:HardDrop(), fall)
		Test.ExpectEqual(match.LockedCount, 1)
		Test.ExpectEqual(match.Board:Get(5, 1), "O")
		Test.ExpectEqual(match.Board:Get(6, 2), "O")
	end)

	Test.Case("reset starts a fresh game", function()
		local match = newMatch(4)
		match.Board:SetRow(1, "XXXX.XXXXX")
		match.Score = 1200
		match.Lines = 14
		match.Level = 2
		match.Hold = "T"
		match.GameOver = true
		match:Reset()
		Test.ExpectEqual(match.Board:Get(1, 1), "")
		Test.ExpectEqual(match.Score, 0)
		Test.ExpectEqual(match.Lines, 0)
		Test.ExpectEqual(match.Level, 1)
		Test.ExpectEqual(match.Hold, "")
		Test.Expect(not match.GameOver)
		Test.ExpectEqual(#match.Queue, Match.PreviewCount)
	end)
end)
