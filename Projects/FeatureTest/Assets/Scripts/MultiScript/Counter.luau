--!strict
-- Adds Step to its total every fixed step (MultiScript.scene). Two slots of one entity may both hold it, each its own
-- instance with its own fields and state (Docs/Decisions/0031-m20-contract.md decision 17).
export type Counter = ScriptInstance & {
	Label: string,
	Step: number,
	Total: number,
}

local Counter = {}

Counter.Fields = {
	Label = Field.String("", { Tooltip = "The name the MultiScript suite tells this instance by" }),
	Step = Field.Integer(1, { Min = 1, Max = 100, Tooltip = "How much the total grows each fixed step" }),
}

function Counter.OnCreate(self: Counter)
	self.Total = 0
end

function Counter.OnFixedUpdate(self: Counter, dt: number)
	self.Total += self.Step
end

return Script.Define("Counter", Counter)
