--!strict
-- Records the 2D physics callbacks of its instance with the tick each arrived at (Physics2D.scene; Docs/Decisions/
-- 0032-m21-contract.md decision 13), so the Physics2D suite reads which partners entered and left and in what order. It
-- keeps the latest MAX_EVENTS only, so a long play (the hardening soak) never grows its heap.
export type Event2D = {
	Kind: string,
	Other: string,
	Tick: number,
	Normal: vector?,
}

export type Contacts2D = ScriptInstance & {
	Events: { Event2D },
}

local MAX_EVENTS = 64

local Contacts2D = {}

-- Appends `event`, dropping the oldest once MAX_EVENTS are kept.
local function record(self: Contacts2D, event: Event2D)
	if #self.Events >= MAX_EVENTS then
		table.remove(self.Events, 1)
	end
	table.insert(self.Events, event)
end

function Contacts2D.OnCreate(self: Contacts2D)
	self.Events = {}
end

function Contacts2D.OnCollisionEnter2D(self: Contacts2D, other: Entity, contact: CollisionContact)
	record(self, { Kind = "CollisionEnter2D", Other = other.Name, Tick = Time.GetTick(), Normal = contact.Normal })
end

function Contacts2D.OnCollisionExit2D(self: Contacts2D, other: Entity)
	record(self, { Kind = "CollisionExit2D", Other = other.Name, Tick = Time.GetTick() })
end

function Contacts2D.OnTriggerEnter2D(self: Contacts2D, other: Entity)
	record(self, { Kind = "TriggerEnter2D", Other = other.Name, Tick = Time.GetTick() })
end

function Contacts2D.OnTriggerExit2D(self: Contacts2D, other: Entity)
	record(self, { Kind = "TriggerExit2D", Other = other.Name, Tick = Time.GetTick() })
end

return Script.Define("Contacts2D", Contacts2D)
