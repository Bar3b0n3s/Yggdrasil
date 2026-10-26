--!strict
-- Journals the callbacks of one script slot under its Label (MultiScript.scene), so an entity with several Tracer slots
-- shows the order in which its slots receive each callback (Architecture §11.3; Docs/Decisions/0031-m20-contract.md
-- decision 17).
local Journal = require("./Journal")

export type Tracer = ScriptInstance & {
	Label: string,
	FixedSteps: number,
}

local Tracer = {}

Tracer.Fields = {
	Label = Field.String("", { Tooltip = "The name the journal records this slot's instance under" }),
}

function Tracer.OnCreate(self: Tracer)
	self.FixedSteps = 0
	Journal.Record(self.Label, "create")
end

function Tracer.OnEnable(self: Tracer)
	Journal.Record(self.Label, "enable")
end

function Tracer.OnDisable(self: Tracer)
	Journal.Record(self.Label, "disable")
end

function Tracer.OnDestroy(self: Tracer)
	Journal.Record(self.Label, "destroy")
end

function Tracer.OnFixedUpdate(self: Tracer, dt: number)
	self.FixedSteps += 1
	Journal.Record(self.Label, "fixed")
end

function Tracer.OnCollisionEnter(self: Tracer, other: Entity)
	Journal.Record(self.Label, "hit:" .. other.Name)
end

return Script.Define("Tracer", Tracer)
