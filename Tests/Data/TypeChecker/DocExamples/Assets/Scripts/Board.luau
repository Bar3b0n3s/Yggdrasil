--!strict
-- A stub of the falling-block board module the documentation's examples require; only its types matter.
export type Board = {
	Width: number,
	Height: number,
	Cells: { string },
	FillRows: (self: Board, first: number, last: number, gap: number) -> (),
	Place: (self: Board, piece: string, column: number, row: number, rotation: number) -> boolean,
	ClearLines: (self: Board) -> number,
	ToAscii: (self: Board) -> string,
}

local Board = {}

local function fillRows(self: Board, first: number, last: number, gap: number)
	for row = first, last do
		for column = 0, self.Width - 1 do
			if column ~= gap then
				self.Cells[row * self.Width + column + 1] = "#"
			end
		end
	end
end

local function place(self: Board, piece: string, column: number, row: number, rotation: number): boolean
	local index = row * self.Width + column + 1
	if self.Cells[index] ~= "." or rotation < 0 then
		return false
	end
	self.Cells[index] = piece
	return true
end

local function clearLines(self: Board): number
	local cleared = 0
	for row = 0, self.Height - 1 do
		local full = true
		for column = 0, self.Width - 1 do
			full = full and self.Cells[row * self.Width + column + 1] ~= "."
		end
		if full then
			cleared += 1
		end
	end
	return cleared
end

local function toAscii(self: Board): string
	return table.concat(self.Cells)
end

function Board.New(): Board
	local board: Board = {
		Width = 10,
		Height = 22,
		Cells = table.create(220, "."),
		FillRows = fillRows,
		Place = place,
		ClearLines = clearLines,
		ToAscii = toAscii,
	}
	return board
end

function Board.Score(lines: number, level: number): number
	local points = { 100, 300, 500, 800 }
	return (points[lines] or 0) * level
end

function Board.GravityTicksForLevel(level: number): number
	return math.max(1, 48 - level * 5)
end

return Board
