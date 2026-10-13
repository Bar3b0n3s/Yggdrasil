--!strict
-- The Text proxy: every field read and written, both spaces and every alignment included.
local Support = require("./Support")

local ALIGNMENTS: { TextAlignment } = { "Left", "Right", "Center" }
local SPACES: { TextSpace } = { "Screen", "World" }

return Test.Suite("Api.Text", function()
	Test.Case("every field reads and writes through the proxy", function()
		local text = Support.Find("Sign").Text :: Text
		Test.ExpectEqual(Support.RoundTrip(text, "Text", "Hello\nworld"), "FeatureTest", "the scene's text")
		Test.ExpectEqual(Support.RoundTrip(text, "Font", nil), nil, "the default font")
		Test.ExpectNear(Support.RoundTrip(text, "Size", 48, 1e-5), 40, 1e-5, "the scene's size")
		local color = Color.New(1, 1, 0, 0.75)
		text.Color = color
		Support.ExpectColorNear(text.Color, color, 1e-6, "Color reads back what was written")
		for _, space in SPACES do
			Support.RoundTrip(text, "Space", space)
		end
		Support.RoundTrip(text, "Anchor", vector.create(0.25, 0.75, 0), 1e-6)
		Support.RoundTrip(text, "Pivot", vector.create(0, 1, 0), 1e-6)
		Support.RoundTrip(text, "Offset", vector.create(12, -8, 0), 1e-5)
		for _, alignment in ALIGNMENTS do
			Support.RoundTrip(text, "Alignment", alignment)
		end
		Support.RoundTrip(text, "Billboard", true)
		Test.Expect(not pcall(function()
			text.Text = string.rep("x", 5000)
		end), "a script writes at most 4096 bytes of text")
		Test.ExpectEqual(text.Text, "Hello\nworld", "a refused write changes nothing")
	end)
end)
