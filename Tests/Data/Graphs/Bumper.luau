--!strict
-- Generated from Assets/Scripts/Bumper.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Strength = Field.Number(5),
	Threshold = Field.Number(1),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
end

function Graph.OnCollisionEnter(self: any, other: Entity, contact: CollisionContact)
	local n3_Result: any
	-- 3: Flow.IfNotNil
	do
		-- 2: Get.Entity.RigidBody
		local v2_Value = other.RigidBody
		self.__GraphLive["2.Value"] = v2_Value
		n3_Result = (v2_Value :: any)
		self.__GraphLive["3.Result"] = n3_Result
		if n3_Result ~= nil then
			-- 4: Call.RigidBody.AddImpulse
			do
				-- 6: Get.CollisionContact.Normal
				local v6_Value = contact.Normal
				self.__GraphLive["6.Value"] = v6_Value
				-- 7: Self.Get.Strength
				local v7_Value = self.Strength
				self.__GraphLive["7.Value"] = v7_Value
				-- 5: Vector.Scale
				local v5_Result = v6_Value * v7_Value
				self.__GraphLive["5.Result"] = v5_Result
				n3_Result:AddImpulse(v5_Result)
			end
			-- 8: Flow.Branch
			do
				-- 10: Get.CollisionContact.RelativeSpeed
				local v10_Value = contact.RelativeSpeed
				self.__GraphLive["10.Value"] = v10_Value
				-- 11: Self.Get.Threshold
				local v11_Value = self.Threshold
				self.__GraphLive["11.Value"] = v11_Value
				-- 9: Compare.Less
				local v9_Result = v10_Value < v11_Value
				self.__GraphLive["9.Result"] = v9_Result
				if v9_Result then
					-- 12: Call.Log.Info
					do
						-- 14: Get.Entity.Name
						local v14_Value = other.Name
						self.__GraphLive["14.Value"] = v14_Value
						-- 13: Text.Concat
						local v13_Result = tostring(v14_Value) .. tostring(" bumped softly")
						self.__GraphLive["13.Result"] = v13_Result
						Log.Info(v13_Result)
					end
				end
			end
		else
			-- 15: Call.Log.Info
			Log.Info("no rigid body")
		end
	end
end

return Script.Define("Bumper", Graph)
