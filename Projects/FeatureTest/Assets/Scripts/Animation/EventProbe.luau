--!strict
-- Records the animation events of its entity's Animator (OnAnimationEvent, M23; Docs/Decisions/0034-m23-contract.md
-- decisions 26 and 31): Animation.scene's Waver plays the Wave clip of Skinned.gltf on two layers, and its import
-- settings give the clip the events Start (0 s), Peak (0.5 s) and End (1 s, its Duration).
export type EventProbe = ScriptInstance & {
	Events: { AnimationEvent },
	Count: number,
}

local EventProbe = {}

-- The most events kept; Count goes on counting.
local KEPT = 64

function EventProbe.OnCreate(self: EventProbe)
	self.Events = {}
	self.Count = 0
end

function EventProbe.OnAnimationEvent(self: EventProbe, event: AnimationEvent)
	self.Count += 1
	if #self.Events < KEPT then
		table.insert(self.Events, event)
	end
end

return Script.Define("EventProbe", EventProbe)
