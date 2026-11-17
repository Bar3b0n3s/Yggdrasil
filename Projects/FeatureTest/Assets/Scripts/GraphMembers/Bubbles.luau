--!strict
-- The menu of GraphMembers.scene (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 12): it counts the clicks that
-- bubble up from its buttons, whose Clicker graphs consume a click with Return.OnUiClick of true (Consuming) or let it
-- bubble with false (Bubbling, whose slot sets the graph's Consume field false).
export type Bubbles = ScriptInstance & {
	Heard: number,
}

local Bubbles = {}

function Bubbles.OnCreate(self: Bubbles)
	self.Heard = 0
end

function Bubbles.OnUiClick(self: Bubbles, element: Entity): boolean?
	self.Heard += 1
	return nil
end

return Script.Define("Bubbles", Bubbles)
