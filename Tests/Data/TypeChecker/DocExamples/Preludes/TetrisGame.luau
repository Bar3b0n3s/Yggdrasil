--!strict
export type Controls = {
	Left: boolean,
	Right: boolean,
	LeftPressed: boolean,
	RightPressed: boolean,
	SoftDrop: boolean,
	HardDrop: boolean,
	RotateCW: boolean,
	RotateCCW: boolean,
	Hold: boolean,
}

export type Match = {
	GameOver: boolean,
	Events: { string },
	Step: (self: Match, controls: Controls) -> (),
	Reset: (self: Match) -> (),
}

export type Game = ScriptInstance & {
	Match: Match,
	Controls: Controls,
	RestartClicked: boolean,
	Restarts: number,
}

local Game = {}

local function onEvent(self: Game, event: string)
	if event == "GameOver" then
		self.RestartClicked = false
	end
end

local function refresh(self: Game)
	self.Controls.Hold = false
end

-- @example

return Script.Define("Game", Game)
