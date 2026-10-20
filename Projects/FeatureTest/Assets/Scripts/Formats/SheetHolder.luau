--!strict
-- Formats.scene's SheetHolder (Architecture §15.5; ADR 0029 decision 22): a behaviour whose Sheet property is a
-- Field.Asset("SpriteSheet"), which the scene sets to Tiles.spritesheet, so a SpriteSheet-typed script field is
-- serialized, exported and resolved in every run mode. M18 adds no script API that reads a sheet's frames.
export type SheetHolder = ScriptInstance & {
	Sheet: AssetRef?,
}

local SheetHolder = {}

SheetHolder.Fields = {
	Sheet = Field.Asset("SpriteSheet"),
}

return Script.Define("SheetHolder", SheetHolder)
