--!strict
-- Direction-following alignment and mirrored rows (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 11 to 13;
-- World Tree's brief 17 item 5) in UiDirectionRows.scene (Scaffold/UiDirection.jsonl, IdSeed 3503), in every run mode, each
-- case in a session of its own: UiText's Start and End on Arabic and Latin labels, and an options row laid out
-- HorizontalReverse, holding a HorizontalReverse slider, above the same row laid out Horizontal (gate 1b: TextAlignment's
-- Start and End and UiLayoutDirection's HorizontalReverse). The golden scaffold GoldenUiDirection.jsonl writes the same
-- scene for the golden image "UiDirection", which shows where the texts are drawn.

local function Find(path: string): Entity
	local entity = Scene.FindByPath(path)
	assert(entity ~= nil, path .. " is in UiDirectionRows.scene")
	return entity
end

-- The laid-out rectangle of the element at `path`, in scene pixels.
local function Rect(path: string): UiElementRect
	local rect = Ui.GetRect(Find(path))
	assert(rect ~= nil, path .. " is laid out")
	return rect
end

-- A tap applies at the next tick and releases at the one after; each helper resumes once both have run.
local function TapKey(key: Key)
	Test.InjectKey(key, "Tap")
	Test.WaitTicks(2)
end

local function ClickAt(position: vector)
	Test.InjectMouse("Left", "Tap", position)
	Test.WaitTicks(2)
end

local CHILDREN = { "Caption", "Volume", "Value" }

return Test.Suite("UiDirection", function()
	Test.Case("a HorizontalReverse row is the Horizontal row mirrored inside its padding", function()
		local reverse = Rect("/Canvas/Reverse")
		local forward = Rect("/Canvas/Forward")
		for _, child in CHILDREN do
			local mirrored = Rect("/Canvas/Reverse/Reverse" .. child)
			local ahead = Rect("/Canvas/Forward/Forward" .. child)
			Test.ExpectNear(mirrored.Min.x - reverse.Min.x, forward.Max.x - ahead.Max.x, 1e-3, child .. " mirrors its place in the Horizontal row")
			Test.ExpectNear(mirrored.Max.x - mirrored.Min.x, ahead.Max.x - ahead.Min.x, 1e-3, child .. " keeps its width")
			Test.ExpectNear(mirrored.Min.y - reverse.Min.y, ahead.Min.y - forward.Min.y, 1e-3, child .. " keeps its place across the row")
		end
		local caption = Rect("/Canvas/Reverse/ReverseCaption")
		local volume = Rect("/Canvas/Reverse/ReverseVolume")
		local value = Rect("/Canvas/Reverse/ReverseValue")
		Test.Expect(caption.Min.x > volume.Max.x and volume.Min.x > value.Max.x, "the first child is at the right, the last at the left")
	end)

	Test.Case("a HorizontalReverse slider fills from its right edge and places its handle at 1 - t", function()
		local volume = Rect("/Canvas/Reverse/ReverseVolume")
		local fill = Rect("/Canvas/Reverse/ReverseVolume/ReverseVolumeFill")
		local handle = Rect("/Canvas/Reverse/ReverseVolume/ReverseVolumeHandle")
		local quarter = volume.Max.x - (volume.Max.x - volume.Min.x) * 0.25
		Test.ExpectNear(fill.Max.x, volume.Max.x, 1e-3, "the fill starts at the right edge, Min's")
		Test.ExpectNear(fill.Min.x, quarter, 1e-3, "and runs a quarter of the way to the left")
		Test.ExpectNear((handle.Min.x + handle.Max.x) / 2, quarter, 1e-3, "the handle sits a quarter of the way from the right")
	end)

	Test.Case("a HorizontalReverse slider steps toward Max with Left and reads the pointer from its right edge", function()
		local volume = Find("/Canvas/Reverse/ReverseVolume")
		local slider = volume.UiSlider :: UiSlider
		Ui.SetFocus(volume)
		TapKey("Left")
		Test.ExpectEqual(slider.Value, 30, "Left steps a HorizontalReverse slider toward Max")
		TapKey("Right")
		TapKey("Right")
		Test.ExpectEqual(slider.Value, 20, "Right steps it toward Min")
		local rect = Rect("/Canvas/Reverse/ReverseVolume")
		ClickAt(vector.create(rect.Max.x - (rect.Max.x - rect.Min.x) * 0.75, (rect.Min.y + rect.Max.y) / 2, 0))
		Test.ExpectNear(slider.Value, 75, 1e-3, "a press three quarters of the way from the right edge")
	end)

	Test.Case("the labels keep Start and End through the load, and the scene renders", function()
		Test.ExpectEqual((Find("/Canvas/ArabicStart").UiText :: UiText).Alignment, "Start")
		Test.ExpectEqual((Find("/Canvas/ArabicEnd").UiText :: UiText).Alignment, "End")
		Test.ExpectEqual((Find("/Canvas/LatinEnd").UiText :: UiText).Alignment, "End")
		Test.WaitTicks(2)
		Test.Screenshot("UiDirection")
	end)
end)
