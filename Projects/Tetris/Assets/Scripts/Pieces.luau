--!strict
-- Tetromino shapes, their four SRS rotation states and the SRS wall-kick tables. Pure data: no engine calls.

-- A block of a piece inside its bounding box: X to the right, Y downwards from the box's top row.
export type Offset = { X: number, Y: number }

export type Shape = {
	Name: string,
	-- The bounding box's width and height.
	Size: number,
	-- States[state + 1] holds the four blocks of rotation state 0 (spawn), 1 (R), 2 or 3 (L).
	States: { { Offset } },
	-- The board column of the box's left edge when the piece spawns.
	SpawnLeft: number,
}

-- A kick: the column shift and the row shift (up-positive) tried for one rotation.
export type Kick = { number }

local Pieces = {}

Pieces.Names = table.freeze({ "I", "O", "T", "S", "Z", "J", "L" })

local function rotateClockwise(blocks: { Offset }, size: number): { Offset }
	local out: { Offset } = {}
	for i, block in blocks do
		out[i] = { X = size - 1 - block.Y, Y = block.X }
	end
	return out
end

local function makeShape(name: string, size: number, spawnLeft: number, blocks: { { number } }): Shape
	local first: { Offset } = {}
	for i, block in blocks do
		first[i] = { X = block[1], Y = block[2] }
	end
	local states: { { Offset } } = { first }
	for state = 2, 4 do
		states[state] = rotateClockwise(states[state - 1], size)
	end
	return { Name = name, Size = size, States = states, SpawnLeft = spawnLeft }
end

local shapes: { [string]: Shape } = {
	I = makeShape("I", 4, 4, { { 0, 1 }, { 1, 1 }, { 2, 1 }, { 3, 1 } }),
	O = makeShape("O", 2, 5, { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } }),
	T = makeShape("T", 3, 4, { { 1, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }),
	S = makeShape("S", 3, 4, { { 1, 0 }, { 2, 0 }, { 0, 1 }, { 1, 1 } }),
	Z = makeShape("Z", 3, 4, { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 2, 1 } }),
	J = makeShape("J", 3, 4, { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }),
	L = makeShape("L", 3, 4, { { 2, 0 }, { 0, 1 }, { 1, 1 }, { 2, 1 } }),
}

-- SRS kicks of the J, L, S, T and Z pieces, by "<from>><to>" rotation state.
local jlstzKicks: { [string]: { Kick } } = {
	["0>1"] = { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
	["1>0"] = { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	["1>2"] = { { 0, 0 }, { 1, 0 }, { 1, -1 }, { 0, 2 }, { 1, 2 } },
	["2>1"] = { { 0, 0 }, { -1, 0 }, { -1, 1 }, { 0, -2 }, { -1, -2 } },
	["2>3"] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
	["3>2"] = { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
	["3>0"] = { { 0, 0 }, { -1, 0 }, { -1, -1 }, { 0, 2 }, { -1, 2 } },
	["0>3"] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, -2 }, { 1, -2 } },
}

-- SRS kicks of the I piece.
local iKicks: { [string]: { Kick } } = {
	["0>1"] = { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
	["1>0"] = { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
	["1>2"] = { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
	["2>1"] = { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
	["2>3"] = { { 0, 0 }, { 2, 0 }, { -1, 0 }, { 2, 1 }, { -1, -2 } },
	["3>2"] = { { 0, 0 }, { -2, 0 }, { 1, 0 }, { -2, -1 }, { 1, 2 } },
	["3>0"] = { { 0, 0 }, { 1, 0 }, { -2, 0 }, { 1, -2 }, { -2, 1 } },
	["0>3"] = { { 0, 0 }, { -1, 0 }, { 2, 0 }, { -1, 2 }, { 2, -1 } },
}

local noKicks: { Kick } = { { 0, 0 } }

-- The shape of a piece by its name ("I", "O", "T", "S", "Z", "J" or "L").
function Pieces.Get(name: string): Shape
	local shape = shapes[name]
	if shape == nil then
		error(`unknown piece '{name}'`)
	end
	return shape
end

-- The kicks tried, in order, when the piece rotates from one state (0 to 3) to another.
function Pieces.Kicks(name: string, from: number, to: number): { Kick }
	if name == "O" then
		return noKicks
	end
	local kicks = if name == "I" then iKicks else jlstzKicks
	return kicks[`{from}>{to}`] or noKicks
end

return Pieces
