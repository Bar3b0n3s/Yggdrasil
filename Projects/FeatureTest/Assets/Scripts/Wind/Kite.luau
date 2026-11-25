--!strict
-- Wind.scene's kite (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3M; Wind.jsonl, IdSeed
-- 4302): every fixed step it pushes its body by the scene's wind where it flies (Wind:Sample at the world's clock) less
-- its own velocity, times Drag, and pulls it back towards where it started by Spring, so it hangs in the wind on a
-- tether. Jump raises the wind's Strength by Gust (up to 100), so the input reaches the wind, the body and the state hash
-- (WindReplay.jsonl's replay, IdSeed 4303).
export type Kite = ScriptInstance & {
	Drag: number,
	Spring: number,
	Gust: number,
	Raises: number,
	Anchor: vector,
}

local Kite = {}

Kite.Fields = {
	Drag = Field.Number(0.6, { Min = 0, Max = 10, Tooltip = "Newtons per m/s of wind against the kite" }),
	Spring = Field.Number(3, { Min = 0, Max = 100, Tooltip = "Newtons per metre back to where it started" }),
	Gust = Field.Number(8, { Min = 0, Max = 50, Tooltip = "Metres per second each Jump adds to the wind's Strength" }),
}

function Kite.OnCreate(self: Kite)
	self.Raises = 0
	self.Anchor = self.Entity.Transform.Translation
end

function Kite.OnFixedUpdate(self: Kite, _dt: number)
	local weather = Scene.FindByName("Weather")
	if weather == nil then
		return
	end
	local wind = weather.Wind
	if wind == nil then
		return
	end
	if Input.IsActionPressed("Jump") then
		wind.Strength = math.min(wind.Strength + self.Gust, 100)
		self.Raises += 1
	end
	local body = self.Entity.RigidBody :: RigidBody
	local position = self.Entity.Transform.WorldPosition
	local relative = wind:Sample(position) - body:GetLinearVelocity()
	body:AddForce(relative * self.Drag + (self.Anchor - position) * self.Spring)
end

return Script.Define("Kite", Kite)
