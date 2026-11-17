--!strict
-- Generated from Assets/Scripts/Valve.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Press = Field.String("/Press"),
}
Graph.RunsWhileWorldPaused = true

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.PressSpeed = nil
	self.Pressure = nil
end

function Graph.OnStart(self: any)
	local n2_Result: any
	local n5_Result: any
	local n8_Result: any
	local n12_Result: any
	-- 2: Flow.IfNotNil
	do
		-- 4: Self.Get.Press
		local v4_Value = self.Press
		self.__GraphLive["4.Value"] = v4_Value
		-- 3: Call.Scene.FindByPath
		local v3_Result = Scene.FindByPath(v4_Value)
		self.__GraphLive["3.Result"] = v3_Result
		n2_Result = (v3_Result :: any)
		self.__GraphLive["2.Result"] = n2_Result
		if n2_Result ~= nil then
			-- 5: Flow.IfNotNil
			do
				-- 6: Call.Entity.GetScript
				local v6_Result: any = n2_Result:GetScript("Press")
				self.__GraphLive["6.Result"] = v6_Result
				n5_Result = v6_Result
				self.__GraphLive["5.Result"] = n5_Result
				if n5_Result ~= nil then
					-- 7: Script.Set
					do
						local o7: any = n5_Result
						o7["Running"] = false
					end
					-- 8: Script.Call
					do
						-- 9: Self.Instance
						local v9_Value: any = self
						self.__GraphLive["9.Value"] = v9_Value
						local o8: any = n5_Result
						n8_Result = (o8:Toggle(v9_Value) :: any)
						self.__GraphLive["8.Result"] = n8_Result
					end
					-- 10: Self.Set.Pressure
					do
						-- 11: Script.Get
						local v11_Value: any = (n8_Result :: any)["Pressure"]
						self.__GraphLive["11.Value"] = v11_Value
						self.Pressure = v11_Value
					end
					-- 12: Script.Call
					do
						local o12: any = n5_Result
						n12_Result = (o12["Speed-Up"](o12, nil, 2) :: any)
						self.__GraphLive["12.Result"] = n12_Result
					end
					-- 14: Self.Set.PressSpeed
					do
						-- 13: Script.Get
						local v13_Value: any = (n5_Result :: any)["Speed"]
						self.__GraphLive["13.Value"] = v13_Value
						self.PressSpeed = v13_Value
					end
				end
			end
		end
	end
end

return Script.Define("Valve", Graph)
