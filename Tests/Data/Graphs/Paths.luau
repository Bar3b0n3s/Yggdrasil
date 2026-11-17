--!strict
-- Generated from Assets/Scripts/Paths.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
end

function Graph.Classify(self: any, p_x: number)
	local result: any
	local function flow6(): boolean
		-- 6: Flow.Branch
		do
			-- 7: Compare.Less
			local v7_Result = p_x < -100
			self.__GraphLive["7.Result"] = v7_Result
			if v7_Result then
				-- 8: Return.Classify
				do
					result = "extreme"
					return true
				end
			end
		end
		return false
	end
	-- 2: Flow.Branch
	do
		-- 3: Compare.Less
		local v3_Result = p_x < 0
		self.__GraphLive["3.Result"] = v3_Result
		if v3_Result then
			if flow6() then return result end
		else
			-- 4: Flow.Branch
			do
				-- 5: Compare.Greater
				local v5_Result = p_x > 100
				self.__GraphLive["5.Result"] = v5_Result
				if v5_Result then
					if flow6() then return result end
				else
					-- 9: Return.Classify
					do
						return "small"
					end
				end
			end
		end
	end
	return nil
end

function Graph.Find(self: any, p_limit: number)
	local n21_Index: number
	-- 21: Flow.ForRange
	for index21 = 1, p_limit do
		n21_Index = index21
		self.__GraphLive["21.Index"] = n21_Index
		-- 22: Flow.Branch
		do
			-- 23: Compare.Equal
			local v23_Result = (n21_Index :: any) == 3
			self.__GraphLive["23.Result"] = v23_Result
			if v23_Result then
				-- 25: Return.Find
				do
					return n21_Index
				end
			end
		end
	end
	-- 24: Return.Find
	do
		return 0
	end
end

function Graph.Nest(self: any, p_a: boolean, p_b: boolean)
	local result: any
	local function flow16(): boolean
		-- 16: Flow.Branch
		if p_b then
			-- 17: Return.Nest
			do
				result = 1
				return true
			end
		end
		return false
	end
	local function flow14(): boolean
		-- 14: Flow.Branch
		do
			-- 15: Compare.Equal
			local v15_Result = (p_a :: any) == p_b
			self.__GraphLive["15.Result"] = v15_Result
			if v15_Result then
				if flow16() then return true end
			end
		end
		return false
	end
	-- 11: Flow.Branch
	if p_a then
		-- 12: Flow.Branch
		if p_b then
			if flow14() then return result end
		else
			if flow14() then return result end
		end
	else
		-- 13: Flow.Branch
		if p_b then
			if flow16() then return result end
		else
			if flow14() then return result end
		end
	end
	return nil
end

function Graph.Pick(self: any, p_flag: boolean)
	local result: any
	local function flow32(): boolean
		-- 32: Return.Pick
		do
			result = p_flag
			return true
		end
	end
	-- 31: Flow.Branch
	if p_flag then
		if flow32() then return result end
	else
		if flow32() then return result end
	end
	return nil
end

return Script.Define("Paths", Graph)
