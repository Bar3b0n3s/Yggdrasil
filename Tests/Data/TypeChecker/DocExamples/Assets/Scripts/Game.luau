--!strict
-- A stub of the falling-block game behaviour the documentation's examples require; only its types matter.
local Board = require("./Board")

export type Game = ScriptInstance & {
	StartLevel: number,
	Board: Board.Board,
	Level: number,
	LockedCount: number,
	GameOver: boolean,
}

local Game = {}

Game.Fields = {
	StartLevel = Field.Integer(1, { Min = 1, Max = 20 }),
}

function Game.OnStart(self: Game)
	self.Board = Board.New()
	self.Level = self.StartLevel
	self.LockedCount = 0
	self.GameOver = false
end

return Script.Define("Game", Game)
