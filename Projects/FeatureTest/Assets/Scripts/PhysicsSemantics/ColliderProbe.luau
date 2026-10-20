--!strict
-- Records the collision and trigger callbacks its entity receives, for the PhysicsSemantics suite and replay
-- (Docs/Decisions/0031-m20-contract.md decisions 2 and 3): each with the names of the entities involved and the tick it
-- arrived on, in the instance's own list and in one log that every probe of the scene's VM shares, in arrival order.

export type Event = {
	Kind: string,
	Self: string,
	Other: string,
	Collider: string,
	OtherCollider: string,
	Tick: number,
}

export type ColliderProbe = ScriptInstance & {
	Events: { Event },
	Log: { Event },
}

local Log: { Event } = {}

local ColliderProbe = {}

local function Record(self: ColliderProbe, kind: string, other: Entity, contact: CollisionContact?)
	local event: Event = {
		Kind = kind,
		Self = self.Entity.Name,
		Other = other.Name,
		Collider = if contact then contact.Collider.Name else "",
		OtherCollider = if contact then contact.OtherCollider.Name else "",
		Tick = Time.GetTick(),
	}
	table.insert(self.Events, event)
	table.insert(Log, event)
end

function ColliderProbe.OnCreate(self: ColliderProbe)
	self.Events = {}
	self.Log = Log
end

function ColliderProbe.OnCollisionEnter(self: ColliderProbe, other: Entity, contact: CollisionContact)
	Record(self, "CollisionEnter", other, contact)
end

function ColliderProbe.OnCollisionExit(self: ColliderProbe, other: Entity)
	Record(self, "CollisionExit", other, nil)
end

function ColliderProbe.OnTriggerEnter(self: ColliderProbe, other: Entity)
	Record(self, "TriggerEnter", other, nil)
end

function ColliderProbe.OnTriggerExit(self: ColliderProbe, other: Entity)
	Record(self, "TriggerExit", other, nil)
end

return Script.Define("ColliderProbe", ColliderProbe)
