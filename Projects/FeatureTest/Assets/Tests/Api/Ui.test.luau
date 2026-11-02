--!strict
-- The UI proxies and Ui.scene (Architecture §15.6 gates 1, 1b and 4; Docs/Decisions/0033-m22-contract.md decision 14):
-- Scaffold/Ui.jsonl wrote every UI component with every field non-default somewhere and every value of every UI enum, so
-- the scene renders through Test.Screenshot in every mode, and every field of UiCanvas, UiRect, UiImage, UiText, UiLayout
-- and UiLayoutElement reads back what the scaffold authored and is written through its proxy, every enum value included.
-- A text draws in a font imported with a non-empty FontImportSettings.Ranges (Docs/Decisions/0033-m22-contract.md
-- decision 6), so the setting is cooked, loaded and drawn in every mode.
local Support = require("./Support")

local TILES = "Assets/Formats/SpriteSheets/Tiles.spritesheet"
local GLYPHS = "Assets/Formats/Fonts/Glyphs.ttf"
-- Inter imported with Ranges: Latin Extended-A (U+0100 to U+017F) besides ASCII and Latin-1.
local RANGES_FONT = "Assets/Ui/Fonts/Inter-Regular.otf"
local EPSILON = 1e-6

local SCALE_MODES: { UiScaleMode } = { "ConstantPixelSize", "MatchHeight" }
local IMAGE_TYPES: { UiImageType } = { "Simple", "Sliced" }
local FILTERS: { UiImageFilter } = { "Point", "Linear" }
local ALIGNMENTS: { TextAlignment } = { "Left", "Center", "Right" }
local VERTICAL_ALIGNMENTS: { UiTextVerticalAlignment } = { "Top", "Middle", "Bottom" }
local KINDS: { UiLayoutKind } = { "Stack", "Grid", "Flex" }
local DIRECTIONS: { UiLayoutDirection } = { "Horizontal", "Vertical" }
local MAIN_ALIGNMENTS: { UiMainAlignment } = { "Start", "Center", "End" }
local CROSS_ALIGNMENTS: { UiCrossAlignment } = { "Start", "Center", "End", "Stretch" }

local function load(path: string): AssetRef
	local asset = Assets.Load(path)
	assert(asset, `{path} loads`)
	return asset
end

-- Writes every value of an enum field in turn, reading each back.
local function roundTripAll<T>(proxy: any, field: string, values: { T })
	for _, value in values do
		Support.RoundTrip(proxy, field, value)
	end
end

return Test.Suite("Api.Ui", function()
	Test.Case("the UI scene renders", function()
		Test.ExpectEqual(Scene.GetName(), "Ui")
		Test.WaitTicks(2)
		Test.Screenshot("Ui")
	end)

	Test.Case("UiCanvas fields read and write through the proxy", function()
		local canvas = Support.Find("Hud").UiCanvas :: UiCanvas
		Test.ExpectEqual(Support.RoundTrip(canvas, "SortOrder", -3), 2, "the scene's sort order")
		Test.ExpectEqual(Support.RoundTrip(canvas, "ScaleFactor", 0.5), 2, "the scene's scale factor")
		Test.ExpectEqual(Support.RoundTrip(canvas, "ReferenceHeight", 1080), 720, "the scene's reference height")
		Test.ExpectEqual(Support.RoundTrip(canvas, "PixelSnap", true), false, "the scene's pixel snap")
		Test.ExpectEqual(canvas.ScaleMode, "MatchHeight", "the scene's scale mode")
		roundTripAll(canvas, "ScaleMode", SCALE_MODES)
		local overlay = Support.Find("Overlay").UiCanvas :: UiCanvas
		Test.ExpectEqual(overlay.ScaleMode, "ConstantPixelSize", "a default canvas keeps a constant pixel size")
	end)

	Test.Case("UiRect fields read and write through the proxy", function()
		local rect = Support.Find("Panel").UiRect :: UiRect
		Test.ExpectNear(Support.RoundTrip(rect, "AnchorMax", vector.create(1, 1, 0), EPSILON), vector.create(0.9, 0.8, 0), EPSILON)
		Test.ExpectNear(Support.RoundTrip(rect, "AnchorMin", vector.create(0.25, 0.5, 0), EPSILON), vector.create(0.1, 0.2, 0), EPSILON)
		Test.ExpectNear(Support.RoundTrip(rect, "Pivot", vector.create(0.5, 0.5, 0), EPSILON), vector.create(0, 1, 0), EPSILON)
		Test.ExpectNear(Support.RoundTrip(rect, "Position", vector.create(-12, 7.5, 0), EPSILON), vector.create(4, -2, 0), EPSILON)
		Test.ExpectNear(Support.RoundTrip(rect, "Size", vector.create(64, 48, 0), EPSILON), vector.create(-8, 0, 0), EPSILON)
		Test.ExpectEqual(Support.RoundTrip(rect, "ClipChildren", false), true, "the scene's ClipChildren")
	end)

	Test.Case("UiImage fields read and write through the proxy", function()
		local image = Support.Find("Panel").UiImage :: UiImage
		local tiles = load(TILES)
		Test.ExpectEqual(Support.RoundTrip(image, "Sheet", nil), tiles, "the scene's sheet")
		Support.RoundTrip(image, "Sheet", tiles)
		Test.ExpectEqual(Support.RoundTrip(image, "Frame", 1), 2, "the scene's frame")
		Support.ExpectColorNear(image.Color, Color.New(1, 0.8, 0.6, 0.9), EPSILON, "the scene's colour")
		local color = Color.New(0.25, 0.5, 0.75, 0.5)
		image.Color = color
		Support.ExpectColorNear(image.Color, color, EPSILON, "Color reads back what was written")
		Test.ExpectEqual(Support.RoundTrip(image, "BorderScale", 0.5), 2, "the scene's border scale")
		Test.ExpectEqual(image.Type, "Sliced", "the scene's image type")
		roundTripAll(image, "Type", IMAGE_TYPES)
		Test.ExpectEqual(image.Filter, "Linear", "the scene's filter")
		roundTripAll(image, "Filter", FILTERS)
	end)

	Test.Case("UiText fields read and write through the proxy", function()
		local text = Support.Find("Label").UiText :: UiText
		Test.ExpectEqual(Support.RoundTrip(text, "Text", "Line one\nline two"), "Score", "the scene's text")
		local glyphs = load(GLYPHS)
		Test.ExpectEqual(Support.RoundTrip(text, "Font", nil), glyphs, "the scene's font")
		Support.RoundTrip(text, "Font", glyphs)
		Test.ExpectEqual(Support.RoundTrip(text, "Size", 48), 24, "the scene's size")
		Support.ExpectColorNear(text.Color, Color.New(1, 1, 0.5, 1), EPSILON, "the scene's colour")
		local color = Color.New(0.5, 0.25, 1, 0.75)
		text.Color = color
		Support.ExpectColorNear(text.Color, color, EPSILON, "Color reads back what was written")
		Test.ExpectEqual(Support.RoundTrip(text, "Wrap", true), false, "the scene's wrap")
		Test.ExpectEqual(Support.RoundTrip(text, "LineSpacing", 2), 1.25, "the scene's line spacing")
		Test.ExpectEqual(text.Alignment, "Center", "the scene's alignment")
		roundTripAll(text, "Alignment", ALIGNMENTS)
		Test.ExpectEqual(text.VerticalAlignment, "Middle", "the scene's vertical alignment")
		roundTripAll(text, "VerticalAlignment", VERTICAL_ALIGNMENTS)
		Test.Expect(not pcall(function()
			text.Text = string.rep("x", 5000)
		end), "a script writes at most 4096 bytes of text")
		Test.ExpectEqual(text.Text, "Line one\nline two", "a refused write changes nothing")
	end)

	Test.Case("a text draws in a font imported with Ranges", function()
		-- The scene renders it in every mode ("the UI scene renders"): its ligatures and Polish letters are Latin
		-- Extended-A's, which only the font's Ranges bake.
		local accent = Support.Find("Accent").UiText :: UiText
		local font = load(RANGES_FONT)
		Test.ExpectEqual(Assets.GetType(font), "Font")
		Test.ExpectEqual(accent.Font, font, "the accent's font")
		Test.ExpectEqual(accent.Text, "Œuvre, cœur, Łódź", "the accent's text")
	end)

	Test.Case("UiLayout fields read and write through the proxy", function()
		local layout = Support.Find("Panel").UiLayout :: UiLayout
		Test.ExpectEqual(layout.Kind, "Flex", "the scene's kind")
		roundTripAll(layout, "Kind", KINDS)
		Test.ExpectEqual(layout.Direction, "Horizontal", "the scene's direction")
		roundTripAll(layout, "Direction", DIRECTIONS)
		local padding = layout.Padding
		Test.ExpectEqual(padding.Left, 4, "the scene's left padding")
		Test.ExpectEqual(padding.Top, 3, "the scene's top padding")
		Test.ExpectEqual(padding.Right, 2, "the scene's right padding")
		Test.ExpectEqual(padding.Bottom, 1, "the scene's bottom padding")
		layout.Padding = { Left = 10, Top = 20, Right = 30, Bottom = 40 }
		local written = layout.Padding
		Test.ExpectEqual(written.Left, 10, "Padding reads back what was written")
		Test.ExpectEqual(written.Top, 20, "Padding reads back what was written")
		Test.ExpectEqual(written.Right, 30, "Padding reads back what was written")
		Test.ExpectEqual(written.Bottom, 40, "Padding reads back what was written")
		Test.ExpectNear(Support.RoundTrip(layout, "Spacing", vector.create(0, 12, 0), EPSILON), vector.create(6, 2, 0), EPSILON)
		Test.ExpectEqual(layout.MainAlignment, "Center", "the scene's main alignment")
		roundTripAll(layout, "MainAlignment", MAIN_ALIGNMENTS)
		Test.ExpectEqual(layout.CrossAlignment, "Stretch", "the scene's cross alignment")
		roundTripAll(layout, "CrossAlignment", CROSS_ALIGNMENTS)
		Test.ExpectEqual(Support.RoundTrip(layout, "Columns", 0), 3, "the scene's columns")
		Test.ExpectNear(Support.RoundTrip(layout, "CellSize", vector.create(64, 64, 0), EPSILON), vector.create(40, 30, 0), EPSILON)
		Test.ExpectEqual(Support.RoundTrip(layout, "Wrap", false), true, "the scene's wrap")
	end)

	Test.Case("UiLayoutElement fields read and write through the proxy", function()
		local element = Support.Find("Icon").UiLayoutElement :: UiLayoutElement
		Test.ExpectEqual(Support.RoundTrip(element, "Grow", 2), 1, "the scene's grow")
		Test.ExpectEqual(Support.RoundTrip(element, "Shrink", 0), 0.5, "the scene's shrink")
		Test.ExpectNear(Support.RoundTrip(element, "MinSize", vector.create(8, 4, 0), EPSILON), vector.create(16, 16, 0), EPSILON)
		Test.ExpectEqual(Support.RoundTrip(element, "IgnoreLayout", true), false, "the scene's IgnoreLayout")
		local label = Support.Find("Label").UiLayoutElement :: UiLayoutElement
		Test.ExpectEqual(label.IgnoreLayout, true, "the label keeps its own placement")
	end)
end)
