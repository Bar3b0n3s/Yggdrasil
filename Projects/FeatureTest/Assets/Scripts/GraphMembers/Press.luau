--!strict
-- The press of GraphMembers.scene (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 12; brief 18's SteamValve): a
-- Luau behaviour whose Running field the Valve graph writes, whose Toggle method the graph calls with its own instance, and
-- whose returned table's Pressure the graph reads. It counts every fixed update in Ticks (it holds while the world is
-- paused) and its strokes while it runs.
export type Press = ScriptInstance & {
	Running: boolean,
	Speed: number,
	Ticks: number,
	Strokes: number,
	By: any,
}

local Press = {}

Press.Fields = {
	Running = Field.Bool(true),
	Speed = Field.Number(2),
}

function Press.OnCreate(self: Press)
	self.Ticks = 0
	self.Strokes = 0
	self.By = nil
end

function Press.OnFixedUpdate(self: Press)
	self.Ticks += 1
	if self.Running then
		self.Strokes += 1
	end
end

-- Records who toggled the press and returns its pressure, a table.
function Press.Toggle(self: Press, by: any): { Pressure: number }
	self.By = by
	return { Pressure = self.Speed * 10 }
end

return Script.Define("Press", Press)
