--!strict
-- The well: 10 columns by 22 rows, column 1 at the left and row 1 at the bottom. Rows 1 to 20 are visible;
-- rows 21 and 22 lie above the top, where pieces spawn. Pure logic: no engine calls.
local Pieces = require("./Pieces")

export type Board = {
	Width: number,
	Height: number,
	Visible: number,
	-- Cells[(row - 1) * Width + column]: "" when empty, else the name of the piece that left the block there.
	Cells: { string },
	Get: (self: Board, column: number, row: number) -> string,
	Fits: (self: Board, name: string, state: number, left: number, top: number) -> boolean,
	Place: (self: Board, name: string, state: number, left: number, top: number) -> boolean,
	ClearLines: (self: Board) -> number,
	SetRow: (self: Board, row: number, pattern: string) -> (),
	Clear: (self: Board) -> (),
	ToAscii: (self: Board) -> string,
}

local Width, Height, Visible = 10, 22, 20

local function get(self: Board, column: number, row: number): string
	return self.Cells[(row - 1) * self.Width + column]
end

-- Inside the well (above the top counts as inside, so a rotation may kick a piece there) and empty.
local function isFree(self: Board, column: number, row: number): boolean
	if column < 1 or column > self.Width or row < 1 then
		return false
	end
	if row > self.Height then
		return true
	end
	return self.Cells[(row - 1) * self.Width + column] == ""
end

-- Whether the piece in that rotation state fits with its box's left column and top row there.
local function fits(self: Board, name: string, state: number, left: number, top: number): boolean
	for _, block in Pieces.Get(name).States[state + 1] do
		if not isFree(self, left + block.X, top - block.Y) then
			return false
		end
	end
	return true
end

-- Writes the piece's blocks inside the well and returns true when the lock tops out: when every block lies above
-- the visible rows (a lock out), or when any block lies above the well (row > Height, where a kick can lift a
-- piece), which the grid cannot hold.
local function place(self: Board, name: string, state: number, left: number, top: number): boolean
	local aboveVisible = true
	local aboveWell = false
	for _, block in Pieces.Get(name).States[state + 1] do
		local column, row = left + block.X, top - block.Y
		if row <= self.Visible then
			aboveVisible = false
		end
		if row > self.Height then
			aboveWell = true
		elseif row >= 1 and column >= 1 and column <= self.Width then
			self.Cells[(row - 1) * self.Width + column] = name
		end
	end
	return aboveVisible or aboveWell
end

-- Removes every full row, moving the rows above it down; returns how many rows it removed.
local function clearLines(self: Board): number
	local width = self.Width
	local cells = self.Cells
	local target = 1
	local cleared = 0
	for row = 1, self.Height do
		local full = true
		for column = 1, width do
			if cells[(row - 1) * width + column] == "" then
				full = false
				break
			end
		end
		if full then
			cleared += 1
		else
			if target ~= row then
				for column = 1, width do
					cells[(target - 1) * width + column] = cells[(row - 1) * width + column]
				end
			end
			target += 1
		end
	end
	for row = target, self.Height do
		for column = 1, width do
			cells[(row - 1) * width + column] = ""
		end
	end
	return cleared
end

-- Fills one row from a pattern of Width characters: "." leaves a cell empty, any other character is a block.
local function setRow(self: Board, row: number, pattern: string)
	for column = 1, self.Width do
		local character = string.sub(pattern, column, column)
		self.Cells[(row - 1) * self.Width + column] = if character == "." or character == "" then "" else character
	end
end

local function clear(self: Board)
	for i = 1, self.Width * self.Height do
		self.Cells[i] = ""
	end
end

-- The visible rows, top first, one line per row: "." for an empty cell, else the block's piece name.
local function toAscii(self: Board): string
	local lines: { string } = {}
	for row = self.Visible, 1, -1 do
		local characters: { string } = {}
		for column = 1, self.Width do
			local cell = self.Cells[(row - 1) * self.Width + column]
			characters[column] = if cell == "" then "." else cell
		end
		table.insert(lines, table.concat(characters))
	end
	return table.concat(lines, "\n")
end

local Board = {}

function Board.New(): Board
	local board: Board = {
		Width = Width,
		Height = Height,
		Visible = Visible,
		Cells = table.create(Width * Height, ""),
		Get = get,
		Fits = fits,
		Place = place,
		ClearLines = clearLines,
		SetRow = setRow,
		Clear = clear,
		ToAscii = toAscii,
	}
	return board
end

return Board
