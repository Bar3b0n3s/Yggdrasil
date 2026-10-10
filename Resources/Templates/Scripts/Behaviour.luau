--!strict
export type {{Name}} = ScriptInstance & {
	Speed: number,
}

local {{Name}} = {}

{{Name}}.Fields = {
	Speed = Field.Number(1, { Min = 0, Tooltip = "Movement speed." }),
}

function {{Name}}.OnFixedUpdate(self: {{Name}}, dt: number)
	self.Entity.Transform:Translate(vector.create(self.Speed * dt, 0, 0))
end

return Script.Define("{{Name}}", {{Name}})
