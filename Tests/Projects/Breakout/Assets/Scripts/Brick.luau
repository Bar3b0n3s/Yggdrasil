--!strict
-- A brick of the wall. The Game behaviour breaks it when the ball hits it and
-- scores its Points; each row of the grid overrides Points and the material.

export type Brick = ScriptInstance & {
	Points: number,
}

local Brick = {}

Brick.Fields = {
	Points = Field.Integer(1, { Min = 1, Tooltip = "The score for breaking this brick." }),
}

return Script.Define("Brick", Brick)
