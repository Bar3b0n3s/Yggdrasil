--!strict
-- Generated from Assets/Scripts/Rune.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Glyphs = Field.Integer(3),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.Blows = 0
	self.LastKind = nil
end

function Graph.OnUiClick(self: any, element: Entity)
	-- 21: Return.OnUiClick
	do
		return true
	end
end

function Graph.Blocks(self: any, p_hit: any)
	local result: any
	local function flow5(): boolean
		-- 5: Call.Log.Info
		Log.Info("blocked")
		-- 6: Return.Blocks
		do
			result = true
			return true
		end
	end
	-- 2: Flow.Branch
	do
		-- 4: Script.Get
		local v4_Value: any = (p_hit :: any)["Kind"]
		self.__GraphLive["4.Value"] = v4_Value
		-- 3: Compare.Equal
		local v3_Result = v4_Value == "Ice"
		self.__GraphLive["3.Result"] = v3_Result
		if v3_Result then
			if flow5() then return result end
		else
			-- 7: Flow.Branch
			do
				-- 4: Script.Get
				v4_Value = ((p_hit :: any)["Kind"] :: any)
				self.__GraphLive["4.Value"] = v4_Value
				-- 8: Compare.Equal
				local v8_Result = v4_Value == "Stone"
				self.__GraphLive["8.Result"] = v8_Result
				if v8_Result then
					if flow5() then return result end
				end
			end
		end
	end
	return nil
end

function Graph.Count(self: any)
	return nil
end

function Graph.Damaged(self: any, p_hit: any, p_amount: number)
	-- 11: Flow.Branch
	do
		-- 12: Compare.Less
		local v12_Result = p_amount < 1
		self.__GraphLive["12.Result"] = v12_Result
		if v12_Result then
			-- 13: Return.Damaged
			do
				return
			end
		else
			-- 14: Self.Set.Blows
			do
				-- 16: Self.Get.Blows
				local v16_Value = self.Blows
				self.__GraphLive["16.Value"] = v16_Value
				-- 15: Number.Add
				local v15_Result = v16_Value + 1
				self.__GraphLive["15.Result"] = v15_Result
				self.Blows = v15_Result
			end
			-- 17: Self.Set.LastKind
			do
				-- 18: Script.Get
				local v18_Value: any = (p_hit :: any)["Kind"]
				self.__GraphLive["18.Value"] = v18_Value
				self.LastKind = v18_Value
			end
		end
	end
end

return Script.Define("Rune", Graph)
