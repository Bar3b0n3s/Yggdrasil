--!strict
-- Counts its fixed steps and its hot reloads (Callbacks.scene; the HotReload suite reloads it with Test.ReloadScript).
local Journal = require("./Journal")

export type Reloadable = ScriptInstance & {
	Step: number,
	Counter: number,
	Reloads: number,
}

local Reloadable = {}

Reloadable.Fields = {
	Step = Field.Integer(1, { Min = 1, Max = 10, Tooltip = "How much the counter grows per fixed step" }),
}

function Reloadable.OnCreate(self: Reloadable)
	self.Counter = 0
	self.Reloads = 0
end

function Reloadable.OnFixedUpdate(self: Reloadable, dt: number)
	self.Counter += self.Step
end

function Reloadable.OnHotReload(self: Reloadable)
	self.Reloads += 1
	Journal.Record("Reloadable", "hot-reload")
end

return Script.Define("Reloadable", Reloadable)
