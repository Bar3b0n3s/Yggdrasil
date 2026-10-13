--!strict
local Board = {}

function Board.GravityTicksForLevel(level: number): number
	return math.max(1, 48 - level * 5)
end

export type Game = ScriptInstance & {
	GameOver: boolean,
	GravityTicks: number,
	Level: number,
	Reset: (self: Game) -> (),
	HandleShift: (self: Game) -> (),
	TryRotate: (self: Game, direction: number) -> boolean,
	TryMove: (self: Game, columns: number, rows: number) -> boolean,
	HardDrop: (self: Game) -> (),
	BeginLockDelay: (self: Game) -> (),
}

local Game = {}

function Game.Reset(self: Game)
	self.GameOver = false
	self.GravityTicks = 0
end

function Game.HandleShift(self: Game) end

function Game.TryRotate(self: Game, direction: number): boolean
	return direction ~= 0
end

function Game.TryMove(self: Game, columns: number, rows: number): boolean
	return columns ~= 0 or rows ~= 0
end

function Game.HardDrop(self: Game) end

function Game.BeginLockDelay(self: Game) end

-- @example

return Script.Define("Game", Game)
