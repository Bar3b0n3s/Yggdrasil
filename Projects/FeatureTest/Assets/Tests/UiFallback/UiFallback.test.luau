--!strict
-- A FeatureTest UI font's fallback chain in every run mode (Docs/Decisions/0033-m22-contract.md decisions 42 and 56, and
-- its milestone review): Scaffold/UiFallback.jsonl imported Noto Sans Hebrew with the Hebrew block's Ranges and a copy of
-- Inter whose FontImportSettings.Fallbacks names it, and UiFallback.scene draws a label mixing Latin and Hebrew in that
-- copy of Inter, so its Hebrew letters come from the fallback and the bidirectional algorithm orders the line. The scene
-- renders through Test.Screenshot in Editor, Release and Dist. No script loads the Hebrew font: an export keeps it through
-- Inter's import dependencies alone, as a game's fallback fonts are kept.
local FONT = "Assets/Ui/FallbackFonts/Inter-Regular.otf"
local TEXT = "Shalom שלום"

return Test.Suite("UiFallback", function()
	Test.Case("a label mixing Latin and Hebrew draws through its font's fallback", function()
		Test.ExpectEqual(Scene.GetName(), "UiFallback")
		local label = Scene.FindByName("Mixed")
		assert(label ~= nil, "Mixed is in UiFallback.scene")
		local text = label.UiText :: UiText
		local font = Assets.Load(FONT)
		assert(font, `{FONT} loads`)
		Test.ExpectEqual(Assets.GetType(font), "Font")
		Test.ExpectEqual(text.Font, font, "the label's font, whose Fallbacks name the Hebrew font")
		Test.ExpectEqual(text.Text, TEXT, "the label's mixed Latin and Hebrew text")
		Test.WaitTicks(2)
		Test.Screenshot("UiFallback")
	end)
end)
