--!strict
-- Localization (M22, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decisions 10, 11 and 14): Localization.jsonl's string
-- table, read through Loc in the project's Localization.Locale, fr-CA, which the table lacks, so its language's strings
-- (fr), and the fallback's (en) for a key without a French string. The suite runs in every mode, so every Loc function is
-- called (gate 2) and the StringTable importer and the .stringtable extension are cooked and loaded (gate 9) in the editor
-- and in the exported Release and Dist games alike.
local TABLE = "Assets/Localization/Menu.stringtable"

return Test.Suite("Localization", function()
	Test.Case("the project's locale reaches Loc", function()
		Test.ExpectEqual(Loc.GetLocale(), "fr-CA")
	end)

	Test.Case("a lookup reads the locale's language, then the fallback", function()
		local menu = Assets.Load(TABLE)
		assert(menu, `{TABLE} loads`)
		Test.ExpectEqual(Assets.GetType(menu), "StringTable")
		Test.ExpectEqual(Loc.Get(menu, "Menu.Start"), "Commencer")
		Test.ExpectEqual(Loc.Get(menu, "Menu.Quit"), "Quit")
	end)

	Test.Case("Has tells a key of the table from one it lacks", function()
		local menu = Assets.Load(TABLE)
		assert(menu, `{TABLE} loads`)
		Test.Expect(Loc.Has(menu, "Menu.Start"), "Menu.Start is a key of the table")
		Test.Expect(not Loc.Has(menu, "Menu.Missing"), "Menu.Missing is not a key of the table")
	end)
end)
