--!strict
-- Drives Water.scene from input (Water.replay; M47, Docs/Decisions/0065-m47-water.md decision 3): Jump raises the storm,
-- the Sea's WaveScale to 2; Fire shoves the Boat along +Z. Written in the fixed step, each is in force at that tick's
-- physics pre-step, where the buoyancy samples the sea. Without input nothing changes.
export type WaterDirector = ScriptInstance & {
	Storms: number,
	Shoves: number,
}

local WaterDirector = {}

function WaterDirector.OnCreate(self: WaterDirector)
	self.Storms = 0
	self.Shoves = 0
end

function WaterDirector.OnFixedUpdate(self: WaterDirector, dt: number)
	if Input.IsActionPressed("Jump") then
		local sea = Scene.FindByName("Sea")
		local water = if sea then sea.Water else nil
		if water then
			self.Storms += 1
			water.WaveScale = 2
		end
	end
	if Input.IsActionPressed("Fire") then
		local boat = Scene.FindByName("Boat")
		local body = if boat then boat.RigidBody else nil
		if body then
			self.Shoves += 1
			body:AddImpulse(vector.create(0, 0, 1200))
		end
	end
end

return Script.Define("WaterDirector", WaterDirector)
