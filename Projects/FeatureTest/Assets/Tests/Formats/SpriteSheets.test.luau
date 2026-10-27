--!strict
-- Formats.scene's sprite sheet (Architecture §15.5; ADR 0029 decision 22): Tiles.spritesheet, which SpriteSheets.jsonl
-- created over Formats.jsonl's Rgba.png, loads with its type and path, so the SpriteSheet importer and the .spritesheet
-- extension are cooked and loaded in every run mode (gate 9), and SheetHolder's Field.Asset("SpriteSheet") property,
-- which the scene sets to the sheet, resolves to the same sheet after the scene's serialization and the export.
local SheetHolderScript = require("../../Scripts/Formats/SheetHolder")

local SHEET = "Assets/Formats/SpriteSheets/Tiles.spritesheet"

return Test.Suite("SpriteSheets", function()
	Test.Case("the sprite sheet loads with its type and path", function()
		local sheet = Assets.Load(SHEET)
		Test.Expect(sheet ~= nil, `{SHEET} loads`)
		if sheet then
			Test.Expect(Assets.IsValid(sheet), SHEET)
			Test.ExpectEqual(Assets.GetType(sheet), "SpriteSheet")
			Test.ExpectEqual(Assets.GetPath(sheet), SHEET)
		end
	end)

	Test.Case("a SpriteSheet script field holds the scene's sheet", function()
		local holder = Scene.FindByName("SheetHolder")
		assert(holder, "Formats.scene has an entity named SheetHolder")
		local instance = holder:GetScript() :: SheetHolderScript.SheetHolder
		Test.Expect(instance.Sheet ~= nil, "the scene sets the Sheet property")
		Test.ExpectEqual(instance.Sheet, Assets.Load(SHEET))
	end)
end)
