--!strict
-- The text input proxy and UiTextInput.scene (Architecture §15.6 gates 1 and 4; Docs/Decisions/0033-m22-contract.md
-- decisions 45 and 55): Scaffold/UiTextInput.jsonl wrote four text inputs with every UiTextInput field non-default
-- somewhere (an input with a MaxLength, one with a Text and a Caret inside it, a disabled one and one that is not
-- Focusable, each with its TextElement and, but for the disabled one, its Placeholder), so the scene renders through
-- Test.Screenshot in every mode (an empty input's placeholder, a filled input's text), and every field reads back what the
-- scaffold authored and is written through the proxy.
local Support = require("./Support")

return Test.Suite("Api.UiTextInput", function()
	Test.Case("the text input scene renders", function()
		Test.ExpectEqual(Scene.GetName(), "UiTextInput")
		Test.WaitTicks(2)
		Test.Screenshot("UiTextInput")
	end)

	Test.Case("UiTextInput fields read and write through the proxy", function()
		local name = Support.Find("Name").UiTextInput :: UiTextInput
		Test.ExpectEqual(name.Interactable, true, "Name is interactable")
		Test.ExpectEqual(name.Focusable, true, "Name is focusable")
		Test.ExpectEqual(Support.RoundTrip(name, "MaxLength", 64), 12, "the scene's MaxLength")
		Test.ExpectEqual(Support.RoundTrip(name, "Text", "Grace"), "", "the scene's Name is empty")
		Test.ExpectEqual(Support.RoundTrip(name, "Caret", 5), 0, "the scene's caret")
		local nameText = Support.Find("NameText")
		Test.ExpectEqual(Support.RoundTrip(name, "TextElement", nil), nameText, "the scene's text element")
		Support.RoundTrip(name, "TextElement", nameText)
		local placeholder = Support.Find("NamePlaceholder")
		Test.ExpectEqual(Support.RoundTrip(name, "Placeholder", nil), placeholder, "the scene's placeholder")
		Support.RoundTrip(name, "Placeholder", placeholder)

		local greeting = Support.Find("Greeting").UiTextInput :: UiTextInput
		Test.ExpectEqual(greeting.Text, "Zoë says hi", "the scene's greeting")
		Test.ExpectEqual(greeting.Caret, 3, "the scene's caret stands after Zoë")
		Test.ExpectEqual(greeting.MaxLength, 256, "the default MaxLength")
		local locked = Support.Find("Locked").UiTextInput :: UiTextInput
		Test.ExpectEqual(Support.RoundTrip(locked, "Interactable", true), false, "the scene's Locked is disabled")
		Test.ExpectEqual(locked.Placeholder, nil, "Locked has no placeholder")
		local code = Support.Find("Code").UiTextInput :: UiTextInput
		Test.ExpectEqual(Support.RoundTrip(code, "Focusable", true), false, "the scene's Code is not focusable")
	end)
end)
