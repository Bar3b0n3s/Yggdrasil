--!strict
-- Records the collision and trigger callbacks of its entity for the Physics suite (Assets/Tests/Physics/Physics.test.luau):
-- each event with the names of the entities involved and the tick it arrived on.

export type Contact = { Other: string, Collider: string, OtherCollider: string, Tick: number }

export type ContactProbe = ScriptInstance & {
	Collisions: { Contact },
	CollisionExits: { string },
	TriggerEnters: { string },
	TriggerExits: { string },
}

local ContactProbe = {}

function ContactProbe.OnCreate(self: ContactProbe)
	self.Collisions = {}
	self.CollisionExits = {}
	self.TriggerEnters = {}
	self.TriggerExits = {}
end

function ContactProbe.OnCollisionEnter(self: ContactProbe, other: Entity, contact: CollisionContact)
	table.insert(self.Collisions, {
		Other = other.Name,
		Collider = contact.Collider.Name,
		OtherCollider = contact.OtherCollider.Name,
		Tick = Time.GetTick(),
	})
end

function ContactProbe.OnCollisionExit(self: ContactProbe, other: Entity)
	table.insert(self.CollisionExits, other.Name)
end

function ContactProbe.OnTriggerEnter(self: ContactProbe, other: Entity)
	table.insert(self.TriggerEnters, other.Name)
end

function ContactProbe.OnTriggerExit(self: ContactProbe, other: Entity)
	table.insert(self.TriggerExits, other.Name)
end

return Script.Define("ContactProbe", ContactProbe)
