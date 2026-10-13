--!strict
-- References into prefab instances from outside them: an instance root, a member of it and the prefab asset to spawn
-- more from (Prefabs.scene's Inspector).
export type Linked = ScriptInstance & {
	Crate: Entity?,
	Target: Entity?,
	Spawnable: AssetRef?,
}

local Linked = {}

Linked.Fields = {
	Crate = Field.Entity(),
	Target = Field.Entity(),
	Spawnable = Field.Asset("Prefab"),
}

return Script.Define("Linked", Linked)
