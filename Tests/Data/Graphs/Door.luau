--!strict
-- Generated from Assets/Scripts/Door.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Player = Field.Entity(),
	Speed = Field.Number(2, { Max = 10, Min = 0 }),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.Opened = false
end

function Graph.OnTriggerEnter(self: any, other: Entity)
	-- 4: Flow.Branch
	do
		-- 3: Self.Get.Player
		local v3_Value = self.Player
		self.__GraphLive["3.Value"] = v3_Value
		-- 2: Compare.Equal
		local v2_Result = other == v3_Value
		self.__GraphLive["2.Result"] = v2_Result
		if v2_Result then
			-- 5: Self.Set.Opened
			self.Opened = true
			-- 6: Call.Log.Info
			Log.Info("Door opened")
		end
	end
end

return Script.Define("Door", Graph)
