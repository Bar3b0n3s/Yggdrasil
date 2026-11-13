--!strict
-- Generated from Assets/Scripts/Counter.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Label = Field.String("Count", { Tooltip = "Shown in the log" }),
	Mode = Field.Enum({ "Up", "Down" }, "Up"),
	Step = Field.Integer(1, { Max = 10, Min = 1 }),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.Count = 0
	self.History = {}
	self.Ready = false
end

function Graph.OnFixedUpdate(self: any, dt: number)
	-- 17: Flow.Branch
	do
		-- 19: Self.Get.Ready
		local v19_Value = self.Ready
		self.__GraphLive["19.Value"] = v19_Value
		-- 7: Self.Get.Count
		local v7_Value = self.Count
		self.__GraphLive["7.Value"] = v7_Value
		-- 21: Compare.LessEqual
		local v21_Result = v7_Value <= 100
		self.__GraphLive["21.Result"] = v21_Result
		-- 24: Self.Get.Mode
		local v24_Value = self.Mode
		self.__GraphLive["24.Value"] = v24_Value
		-- 23: Compare.NotEqual
		local v23_Result = v24_Value ~= "Up"
		self.__GraphLive["23.Result"] = v23_Result
		-- 22: Logic.Not
		local v22_Result = not v23_Result
		self.__GraphLive["22.Result"] = v22_Result
		-- 20: Logic.Or
		local v20_Result = v21_Result or v22_Result
		self.__GraphLive["20.Result"] = v20_Result
		-- 18: Logic.And
		local v18_Result = v19_Value and v20_Result
		self.__GraphLive["18.Result"] = v18_Result
		if v18_Result then
			-- 25: Self.Set.Count
			do
				-- 7: Self.Get.Count
				v7_Value = self.Count
				self.__GraphLive["7.Value"] = v7_Value
				-- 4: Self.Get.Step
				local v4_Value = self.Step
				self.__GraphLive["4.Value"] = v4_Value
				-- 29: Number.Multiply
				local v29_Result = v4_Value * dt
				self.__GraphLive["29.Result"] = v29_Result
				-- 27: Number.Add
				local v27_Result = v7_Value + v29_Result
				self.__GraphLive["27.Result"] = v27_Result
				-- 30: Number.Divide
				local v30_Result = v7_Value / 2
				self.__GraphLive["30.Result"] = v30_Result
				-- 28: Number.Modulo
				local v28_Result = v30_Result % 1
				self.__GraphLive["28.Result"] = v28_Result
				-- 26: Number.Subtract
				local v26_Result = v27_Result - v28_Result
				self.__GraphLive["26.Result"] = v26_Result
				self.Count = v26_Result
			end
		else
			-- 32: Flow.Branch
			do
				-- 7: Self.Get.Count
				v7_Value = self.Count
				self.__GraphLive["7.Value"] = v7_Value
				-- 31: Number.Negate
				local v31_Result = -dt
				self.__GraphLive["31.Result"] = v31_Result
				-- 33: Compare.Greater
				local v33_Result = v7_Value > v31_Result
				self.__GraphLive["33.Result"] = v33_Result
				if v33_Result then
					-- 34: Self.Set.Count
					self.Count = 0
				end
			end
		end
	end
end

function Graph.OnStart(self: any)
	local n3_Index: number
	local n11_Element: any
	local n11_Index: number
	local function flow8()
		-- 8: Call.Log.Info
		do
			-- 10: Self.Get.Label
			local v10_Value = self.Label
			self.__GraphLive["10.Value"] = v10_Value
			-- 7: Self.Get.Count
			local v7_Value = self.Count
			self.__GraphLive["7.Value"] = v7_Value
			-- 9: Text.Concat
			local v9_Result = tostring(v10_Value) .. tostring(v7_Value)
			self.__GraphLive["9.Result"] = v9_Result
			Log.Info(v9_Result)
		end
	end
	-- 2: Flow.Sequence
	-- 3: Flow.ForRange
	do
		-- 4: Self.Get.Step
		local v4_Value = self.Step
		self.__GraphLive["4.Value"] = v4_Value
		for index3 = 1, v4_Value do
			n3_Index = index3
			self.__GraphLive["3.Index"] = n3_Index
			-- 5: Self.Set.Count
			do
				-- 7: Self.Get.Count
				local v7_Value = self.Count
				self.__GraphLive["7.Value"] = v7_Value
				-- 6: Number.Add
				local v6_Result = v7_Value + n3_Index
				self.__GraphLive["6.Result"] = v6_Result
				self.Count = v6_Result
			end
		end
	end
	flow8()
	-- 11: Flow.ForEach
	do
		-- 12: Self.Get.History
		local v12_Value: any = self.History
		self.__GraphLive["12.Value"] = v12_Value
		for index11, element11 in ipairs(v12_Value) do
			n11_Element = element11
			n11_Index = index11
			self.__GraphLive["11.Element"] = n11_Element
			self.__GraphLive["11.Index"] = n11_Index
			-- 13: Call.Log.Info
			Log.Info(n11_Element)
		end
	end
	flow8()
	-- 14: Flow.Delay
	Task.Delay(0.5, function()
		-- 15: Self.Set.Ready
		self.Ready = true
	end)
end

return Script.Define("Counter", Graph)
