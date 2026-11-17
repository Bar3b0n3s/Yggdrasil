--!strict
-- The game's flow of GraphMembers.scene (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 12; World Tree's brief
-- 18's GameFlow): a Luau behaviour whose Paused member the Lamp graph reads through the instance chain (Scene.FindByPath,
-- Flow.IfNotNil, Entity:GetScript, Script.Get), as every interactable of the game asks GameFlow whether it is paused.
export type GameFlow = ScriptInstance & {
	Paused: boolean,
}

local GameFlow = {}

function GameFlow.OnCreate(self: GameFlow)
	self.Paused = false
end

return Script.Define("GameFlow", GameFlow)
