--!strict
-- Journals every lifecycle and physics callback of its instance under its Label (Callbacks.scene, §11.3).
local Journal = require("./Journal")

export type Recorder = ScriptInstance & {
	Label: string,
	Created: number,
	FixedSteps: number,
}

local Recorder = {}

Recorder.Fields = {
	Label = Field.String("", { Tooltip = "The name the journal records this instance under" }),
}

function Recorder.OnCreate(self: Recorder)
	self.Created = Time.GetTick()
	self.FixedSteps = 0
	Journal.Labels[self.Entity] = self.Label
	Journal.Record(self.Label, "create")
end

function Recorder.OnStart(self: Recorder)
	Journal.Record(self.Label, "start")
end

function Recorder.OnEnable(self: Recorder)
	Journal.Record(self.Label, "enable")
end

function Recorder.OnDisable(self: Recorder)
	Journal.Record(self.Label, "disable")
end

function Recorder.OnDestroy(self: Recorder)
	Journal.Record(self.Label, "destroy")
end

function Recorder.OnFixedUpdate(self: Recorder, dt: number)
	self.FixedSteps += 1
	if Journal.TracePhases then
		Journal.Record(self.Label, "fixed")
	end
end

function Recorder.OnUpdate(self: Recorder, dt: number)
	if Journal.TracePhases then
		Journal.Record(self.Label, "update")
	end
end

function Recorder.OnLateUpdate(self: Recorder, dt: number)
	if Journal.TracePhases then
		Journal.Record(self.Label, "late")
	end
end

function Recorder.OnCollisionEnter(self: Recorder, other: Entity, contact: CollisionContact)
	Journal.Contacts[self.Label] = contact
	Journal.Record(self.Label, "collision-enter:" .. Journal.LabelOf(other))
end

function Recorder.OnCollisionExit(self: Recorder, other: Entity)
	Journal.Record(self.Label, "collision-exit:" .. Journal.LabelOf(other))
end

function Recorder.OnTriggerEnter(self: Recorder, other: Entity)
	Journal.Record(self.Label, "trigger-enter:" .. Journal.LabelOf(other))
end

function Recorder.OnTriggerExit(self: Recorder, other: Entity)
	Journal.Record(self.Label, "trigger-exit:" .. Journal.LabelOf(other))
end

return Script.Define("Recorder", Recorder)
