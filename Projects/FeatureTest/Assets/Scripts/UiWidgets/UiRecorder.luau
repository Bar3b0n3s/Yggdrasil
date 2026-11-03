--!strict
-- Records the UI events that reach UiWidgets.scene's Menu canvas (Docs/Decisions/0033-m22-contract.md decisions 27 and
-- 36): every UI event bubbles from its element through its ancestors to the canvas, where this behaviour receives it, so
-- the UiWidgets suite reads what its injected input raised, and Assets/Tests/Replays/Ui.replay's expectations what its
-- input changed. It returns nothing, so no event stops here (nothing is above the canvas anyway). The log keeps only the
-- latest Capacity entries, in a ring, and each tally one entry per element name, so a long session (the hardening soak
-- plays the scene for 10,000 ticks) does not grow the script heap; reading entries the ring has dropped is an error.
export type UiRecorder = ScriptInstance & {
	Count: number,
	Ring: { string },
	Clicks: { [string]: number },
	Cancels: { [string]: number },
	Values: { [string]: boolean | number },
	Scrolls: { [string]: vector },
	Since: (self: UiRecorder, cursor: number) -> string,
}

local UiRecorder = {}

UiRecorder.Capacity = 64

local function Record(self: UiRecorder, entry: string)
	self.Count += 1
	self.Ring[(self.Count - 1) % UiRecorder.Capacity + 1] = entry
end

function UiRecorder.OnCreate(self: UiRecorder)
	self.Count = 0
	self.Ring = {}
	self.Clicks = {}
	self.Cancels = {}
	self.Values = {}
	self.Scrolls = {}
end

function UiRecorder.OnUiClick(self: UiRecorder, element: Entity): boolean?
	self.Clicks[element.Name] = (self.Clicks[element.Name] or 0) + 1
	Record(self, `Click:{element.Name}`)
	return nil
end

function UiRecorder.OnUiValueChanged(self: UiRecorder, element: Entity, value: boolean | number): boolean?
	self.Values[element.Name] = value
	Record(self, `Value:{element.Name}:{tostring(value)}`)
	return nil
end

function UiRecorder.OnUiFocus(self: UiRecorder, element: Entity, focused: boolean): boolean?
	Record(self, `Focus:{element.Name}:{tostring(focused)}`)
	return nil
end

function UiRecorder.OnUiHover(self: UiRecorder, element: Entity, hovered: boolean): boolean?
	Record(self, `Hover:{element.Name}:{tostring(hovered)}`)
	return nil
end

function UiRecorder.OnUiCancel(self: UiRecorder, element: Entity): boolean?
	self.Cancels[element.Name] = (self.Cancels[element.Name] or 0) + 1
	Record(self, `Cancel:{element.Name}`)
	return nil
end

function UiRecorder.OnUiScroll(self: UiRecorder, element: Entity, position: vector): boolean?
	self.Scrolls[element.Name] = position
	Record(self, `Scroll:{element.Name}:{position.x},{position.y}`)
	return nil
end

-- The entries recorded after `cursor` (a Count read earlier), joined with commas.
function UiRecorder.Since(self: UiRecorder, cursor: number): string
	if cursor < self.Count - UiRecorder.Capacity then
		error(`UiRecorder: the entries after cursor {cursor} were dropped; only the latest {UiRecorder.Capacity} are kept`)
	end
	local entries: { string } = {}
	for index = cursor + 1, self.Count do
		table.insert(entries, self.Ring[(index - 1) % UiRecorder.Capacity + 1])
	end
	return table.concat(entries, ",")
end

return Script.Define("UiRecorder", UiRecorder)
