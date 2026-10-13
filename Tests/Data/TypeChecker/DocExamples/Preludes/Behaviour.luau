--!strict
export type Example = ScriptInstance & {
	Speed: number,
	Count: number,
}

local Example = {}

Example.Fields = {
	Speed = Field.Number(1, { Min = 0 }),
}

-- @example

return Script.Define("Example", Example)
