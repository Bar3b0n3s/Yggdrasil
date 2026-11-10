--!strict
-- Tilemaps.scene's Level (Docs/Decisions/0032-m21-contract.md decisions 23 and 32): a behaviour whose Level property is a
-- Field.Asset("Tilemap"), which the scene sets to Level.tilemap, so a Tilemap-typed script field is serialized, exported
-- and resolved in every run mode.
export type LevelProbe = ScriptInstance & {
	Level: AssetRef?,
}

local LevelProbe = {}

LevelProbe.Fields = {
	Level = Field.Asset("Tilemap"),
}

return Script.Define("LevelProbe", LevelProbe)
