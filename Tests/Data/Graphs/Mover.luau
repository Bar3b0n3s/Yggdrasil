--!strict
-- Generated from Assets/Scripts/Mover.graph by the graph transpiler: edit the graph, not this text.
local Graph = {}

Graph.Fields = {
	Direction = Field.Vector(vector.create(1, 0, 0)),
	Speed = Field.Number(2, { Max = 10, Min = 0, Step = 0.5 }),
	Spin = Field.Quat(Quat.New(0, 0, 0, 1)),
	Tint = Field.Color(Color.New(1, 0.5, 0.25, 1)),
}

function Graph.OnCreate(self: any)
	self.__GraphLive = {}
	self.Distance = 0
	self.Origin = vector.create(0, 0, 0)
end

function Graph.OnStart(self: any)
	-- 2: Self.Set.Origin
	do
		-- 5: Self.Entity
		local v5_Value = self.Entity
		self.__GraphLive["5.Value"] = v5_Value
		-- 3: Get.Entity.Transform
		local v3_Value = v5_Value.Transform
		self.__GraphLive["3.Value"] = v3_Value
		-- 4: Get.Transform.Translation
		local v4_Value = v3_Value.Translation
		self.__GraphLive["4.Value"] = v4_Value
		self.Origin = v4_Value
	end
end

function Graph.OnUpdate(self: any, dt: number)
	-- 7: Set.Transform.Translation
	do
		-- 5: Self.Entity
		local v5_Value = self.Entity
		self.__GraphLive["5.Value"] = v5_Value
		-- 3: Get.Entity.Transform
		local v3_Value = v5_Value.Transform
		self.__GraphLive["3.Value"] = v3_Value
		-- 4: Get.Transform.Translation
		local v4_Value = v3_Value.Translation
		self.__GraphLive["4.Value"] = v4_Value
		-- 12: Self.Get.Direction
		local v12_Value = self.Direction
		self.__GraphLive["12.Value"] = v12_Value
		-- 10: Vector.Normalize
		local v10_Result = vector.normalize(v12_Value)
		self.__GraphLive["10.Result"] = v10_Result
		-- 13: Self.Get.Speed
		local v13_Value = self.Speed
		self.__GraphLive["13.Value"] = v13_Value
		-- 11: Number.Multiply
		local v11_Result = v13_Value * dt
		self.__GraphLive["11.Result"] = v11_Result
		-- 9: Vector.Scale
		local v9_Result = v10_Result * v11_Result
		self.__GraphLive["9.Result"] = v9_Result
		-- 8: Vector.Add
		local v8_Result = v4_Value + v9_Result
		self.__GraphLive["8.Result"] = v8_Result
		v3_Value.Translation = v8_Result
	end
	-- 14: Self.Set.Distance
	do
		-- 5: Self.Entity
		local v5_Value = self.Entity
		self.__GraphLive["5.Value"] = v5_Value
		-- 3: Get.Entity.Transform
		local v3_Value = v5_Value.Transform
		self.__GraphLive["3.Value"] = v3_Value
		-- 4: Get.Transform.Translation
		local v4_Value = v3_Value.Translation
		self.__GraphLive["4.Value"] = v4_Value
		-- 17: Self.Get.Origin
		local v17_Value = self.Origin
		self.__GraphLive["17.Value"] = v17_Value
		-- 16: Vector.Subtract
		local v16_Result = v4_Value - v17_Value
		self.__GraphLive["16.Result"] = v16_Result
		-- 15: Vector.Length
		local v15_Result = vector.magnitude(v16_Result)
		self.__GraphLive["15.Result"] = v15_Result
		self.Distance = v15_Result
	end
	-- 18: Set.Transform.Rotation
	do
		-- 5: Self.Entity
		local v5_Value = self.Entity
		self.__GraphLive["5.Value"] = v5_Value
		-- 3: Get.Entity.Transform
		local v3_Value = v5_Value.Transform
		self.__GraphLive["3.Value"] = v3_Value
		-- 19: Self.Get.Spin
		local v19_Value = self.Spin
		self.__GraphLive["19.Value"] = v19_Value
		v3_Value.Rotation = v19_Value
	end
	-- 20: Call.Debug.DrawLine
	do
		-- 5: Self.Entity
		local v5_Value = self.Entity
		self.__GraphLive["5.Value"] = v5_Value
		-- 3: Get.Entity.Transform
		local v3_Value = v5_Value.Transform
		self.__GraphLive["3.Value"] = v3_Value
		-- 4: Get.Transform.Translation
		local v4_Value = v3_Value.Translation
		self.__GraphLive["4.Value"] = v4_Value
		-- 12: Self.Get.Direction
		local v12_Value = self.Direction
		self.__GraphLive["12.Value"] = v12_Value
		-- 25: Vector.Cross
		local v25_Result = vector.cross(v12_Value, vector.create(0, 1, 0))
		self.__GraphLive["25.Result"] = v25_Result
		-- 23: Vector.Split
		local v23_X = v25_Result.x
		local v23_Y = v25_Result.y
		local v23_Z = v25_Result.z
		self.__GraphLive["23.X"] = v23_X
		self.__GraphLive["23.Y"] = v23_Y
		self.__GraphLive["23.Z"] = v23_Z
		-- 24: Vector.Dot
		local v24_Result = vector.dot(v12_Value, v25_Result)
		self.__GraphLive["24.Result"] = v24_Result
		-- 21: Vector.Make
		local v21_Result = vector.create(v23_X, 2, v24_Result)
		self.__GraphLive["21.Result"] = v21_Result
		-- 22: Self.Get.Tint
		local v22_Value = self.Tint
		self.__GraphLive["22.Value"] = v22_Value
		Debug.DrawLine(v4_Value, v21_Result, v22_Value)
	end
	-- 26: Flow.Branch
	do
		-- 28: Self.Get.Distance
		local v28_Value = self.Distance
		self.__GraphLive["28.Value"] = v28_Value
		-- 29: Const.Math.Pi
		local v29_Value = Math.Pi
		self.__GraphLive["29.Value"] = v29_Value
		-- 27: Compare.GreaterEqual
		local v27_Result = v28_Value >= v29_Value
		self.__GraphLive["27.Result"] = v27_Result
		if v27_Result then
			-- 30: Call.Transform.Rotate
			do
				-- 5: Self.Entity
				local v5_Value = self.Entity
				self.__GraphLive["5.Value"] = v5_Value
				-- 3: Get.Entity.Transform
				local v3_Value = v5_Value.Transform
				self.__GraphLive["3.Value"] = v3_Value
				v3_Value:Rotate(vector.create(0, 90, 0))
			end
		end
	end
end

return Script.Define("Mover", Graph)
