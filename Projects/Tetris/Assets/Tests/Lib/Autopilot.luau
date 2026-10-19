--!strict
-- A placement autopilot for the recording suites: it reads the match, scores every resting place of the falling
-- piece reachable from where it is now with a classic heuristic (lines, aggregate height, holes, bumpiness) and
-- returns the best rotation state and box column. Pure logic on script tables: it never touches the scene.
local Board = require("../../Scripts/Board")
local Match = require("../../Scripts/Match")

export type Plan = { State: number, Left: number, Score: number }

local Autopilot = {}

local function evaluate(board: Board.Board): number
	local aggregate, holes, bumpiness = 0, 0, 0
	local previous = -1
	for column = 1, board.Width do
		local height = 0
		for row = board.Height, 1, -1 do
			if board:Get(column, row) ~= "" then
				height = row
				break
			end
		end
		for row = 1, height - 1 do
			if board:Get(column, row) == "" then
				holes += 1
			end
		end
		aggregate += height
		if previous >= 0 then
			bumpiness += math.abs(height - previous)
		end
		previous = height
	end
	return -0.51 * aggregate - 0.36 * holes - 0.18 * bumpiness
end

-- The best place for the match's falling piece, or nil when it fits nowhere.
function Autopilot.Choose(match: Match.Match): Plan?
	local best: Plan? = nil
	local states = if match.Piece == "O" then 1 else 4
	local scratch = Board.New()
	local cells = match.Board.Cells
	for state = 0, states - 1 do
		for left = -3, 10 do
			if match.Board:Fits(match.Piece, state, left, match.Top) then
				table.move(cells, 1, #cells, 1, scratch.Cells)
				local top = match.Top
				while scratch:Fits(match.Piece, state, left, top - 1) do
					top -= 1
				end
				scratch:Place(match.Piece, state, left, top)
				local lines = scratch:ClearLines()
				local score = 0.76 * lines + evaluate(scratch)
				if best == nil or score > (best :: Plan).Score then
					best = { State = state, Left = left, Score = score }
				end
			end
		end
	end
	return best
end

return Autopilot
