--!strict
-- Records the UI events that reach UiWorld.scene's Board, a World canvas (Docs/Decisions/0033-m22-contract.md decisions 50
-- and 55): every UI event bubbles from its element through its ancestors to the canvas, where this behaviour receives it,
-- so the UiWorld suite reads what its pointer input, cast through the camera, raised, and
-- Assets/Tests/Replays/UiWorld.replay's expectations what its input changed. It returns nothing, so no event stops here.
-- The log keeps only the latest Capacity entries, in a ring, and each tally one entry per element name, so a long session
-- (the hardening soak plays the scene for 10,000 ticks) does not grow the script heap; reading entries the ring has
-- dropped is an error.
export type WorldRecorder = ScriptInstance & {
	Count: number,
	Ring: { string },
	Clicks: { [string]: number },
	Values: { [string]: boolean | number },
	Since: (self: WorldRecorder, cursor: number) -> string,
}

local WorldRecorder = {}

WorldRecorder.Capacity = 64

local function Record(self: WorldRecorder, entry: string)
	self.Count += 1
	self.Ring[(self.Count - 1) % WorldRecorder.Capacity + 1] = entry
end

function WorldRecorder.OnCreate(self: WorldRecorder)
	self.Count = 0
	self.Ring = {}
	self.Clicks = {}
	self.Values = {}
end

function WorldRecorder.OnUiClick(self: WorldRecorder, element: Entity): boolean?
	self.Clicks[element.Name] = (self.Clicks[element.Name] or 0) + 1
	Record(self, `Click:{element.Name}`)
	return nil
end

function WorldRecorder.OnUiValueChanged(self: WorldRecorder, element: Entity, value: boolean | number): boolean?
	self.Values[element.Name] = value
	Record(self, `Value:{element.Name}:{tostring(value)}`)
	return nil
end

function WorldRecorder.OnUiFocus(self: WorldRecorder, element: Entity, focused: boolean): boolean?
	Record(self, `Focus:{element.Name}:{tostring(focused)}`)
	return nil
end

function WorldRecorder.OnUiHover(self: WorldRecorder, element: Entity, hovered: boolean): boolean?
	Record(self, `Hover:{element.Name}:{tostring(hovered)}`)
	return nil
end

-- The entries recorded after `cursor` (a Count read earlier), joined with commas.
function WorldRecorder.Since(self: WorldRecorder, cursor: number): string
	if cursor < self.Count - WorldRecorder.Capacity then
		error(`WorldRecorder: the entries after cursor {cursor} were dropped; only the latest {WorldRecorder.Capacity} are kept`)
	end
	local entries: { string } = {}
	for index = cursor + 1, self.Count do
		table.insert(entries, self.Ring[(index - 1) % WorldRecorder.Capacity + 1])
	end
	return table.concat(entries, ",")
end

return Script.Define("WorldRecorder", WorldRecorder)
