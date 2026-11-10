--!strict
-- Records what reaches UiTextInput.scene's Form canvas (Docs/Decisions/0033-m22-contract.md decisions 46, 47 and 55):
-- every UI event bubbles from its element through its ancestors to the canvas, where this behaviour receives it, and each
-- fixed step logs the committed characters its latched view holds (Input.GetText), so the UiTextInput suite reads what its
-- injected input raised and typed, and Assets/Tests/Replays/TextInput.replay's expectations what its input changed. In a
-- tick the UI events are delivered before OnFixedUpdate, so an edit's Changed entry precedes its Typed entry. It returns
-- nothing, so no event stops here. The log keeps only the latest Capacity entries, in a ring, and each tally one entry per
-- element name, so a long session (the hardening soak plays the scene for 10,000 ticks) does not grow the script heap;
-- reading entries the ring has dropped is an error.
export type TextRecorder = ScriptInstance & {
	Count: number,
	Ring: { string },
	Changes: { [string]: number },
	Submits: { [string]: string },
	Since: (self: TextRecorder, cursor: number) -> string,
}

local TextRecorder = {}

TextRecorder.Capacity = 64

local function Record(self: TextRecorder, entry: string)
	self.Count += 1
	self.Ring[(self.Count - 1) % TextRecorder.Capacity + 1] = entry
end

function TextRecorder.OnCreate(self: TextRecorder)
	self.Count = 0
	self.Ring = {}
	self.Changes = {}
	self.Submits = {}
end

function TextRecorder.OnFixedUpdate(self: TextRecorder, _dt: number)
	local typed = Input.GetText()
	if typed ~= "" then
		Record(self, `Typed:{typed}`)
	end
end

function TextRecorder.OnUiTextChanged(self: TextRecorder, element: Entity, text: string): boolean?
	self.Changes[element.Name] = (self.Changes[element.Name] or 0) + 1
	Record(self, `Changed:{element.Name}:{text}`)
	return nil
end

function TextRecorder.OnUiSubmit(self: TextRecorder, element: Entity, text: string): boolean?
	self.Submits[element.Name] = text
	Record(self, `Submit:{element.Name}:{text}`)
	return nil
end

function TextRecorder.OnUiFocus(self: TextRecorder, element: Entity, focused: boolean): boolean?
	Record(self, `Focus:{element.Name}:{tostring(focused)}`)
	return nil
end

function TextRecorder.OnUiHover(self: TextRecorder, element: Entity, hovered: boolean): boolean?
	Record(self, `Hover:{element.Name}:{tostring(hovered)}`)
	return nil
end

function TextRecorder.OnUiScroll(self: TextRecorder, element: Entity, position: vector): boolean?
	Record(self, `Scroll:{element.Name}:{position.x},{position.y}`)
	return nil
end

-- The entries recorded after `cursor` (a Count read earlier), joined with commas.
function TextRecorder.Since(self: TextRecorder, cursor: number): string
	if cursor < self.Count - TextRecorder.Capacity then
		error(`TextRecorder: the entries after cursor {cursor} were dropped; only the latest {TextRecorder.Capacity} are kept`)
	end
	local entries: { string } = {}
	for index = cursor + 1, self.Count do
		table.insert(entries, self.Ring[(index - 1) % TextRecorder.Capacity + 1])
	end
	return table.concat(entries, ",")
end

return Script.Define("TextRecorder", TextRecorder)
