--!strict
-- Tilemaps.scene's Goal (Docs/Decisions/0032-m21-contract.md decisions 25 and 32): a trigger TilemapCollider whose
-- merged boxes make one implicit sensor body; it counts the bodies that enter and leave it, by name, so the suites and
-- the Tilemap replay read when the ball reached the goal tiles.
export type GoalZone = ScriptInstance & {
	Entered: number,
	Left: number,
	Last: string,
}

local GoalZone = {}

function GoalZone.OnCreate(self: GoalZone)
	self.Entered = 0
	self.Left = 0
	self.Last = ""
end

function GoalZone.OnTriggerEnter2D(self: GoalZone, other: Entity)
	self.Entered += 1
	self.Last = other.Name
end

function GoalZone.OnTriggerExit2D(self: GoalZone, _other: Entity)
	self.Left += 1
end

return Script.Define("GoalZone", GoalZone)
