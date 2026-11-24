# Water in scripts

Water (M47, Architecture §9.12) is a `Water` component: an `Ocean` (all of X and Z and everything below its surface, one a
scene) or a `Rectangle` (`Size` along the entity's local X and Z, centred on it, from `Depth` below its base to `Headroom`
above it). Its surface is its entity's world height plus up to eight Gerstner `Waves`, times `WaveScale`, at the world's
time: scripts, the physics and the renderer sample the same surface, so a boat floats on the waves it is drawn on.

## Reading the surface

- `Physics.SampleWater(position, time?)` answers for the water that holds a point: the `Rectangle` whose volume holds it
  with the highest surface there, else the scene's `Ocean`, else nil. It returns a `WaterSample`: `Water` (the entity),
  `Height`, `Normal`, `Velocity` (the waves' motion plus the water's `Current`) and `Depth`, the height minus the
  point's y, positive under water. Inside a `Rectangle`'s volume the `Ocean` is masked: a cave lake whose `Headroom`
  reaches the cave's ceiling is dry above its own surface, even far below the sea's level.
- `water:SampleHeight(position, time?)`, `SampleNormal`, `SampleVelocity` and `IsUnderwater` read one water's own surface,
  other waters' masks aside; a `Rectangle`'s are nil outside its footprint.
- `time` defaults to `Time.GetTime()`: the world's time, held by `Time.PauseWorld`, and 0 in an edit scene. Read in
  `OnFixedUpdate`, a sample is the surface the physics floats bodies on that tick.

## The tide and the storm

Write the water entity's `Transform.Translation` in the fixed step to move its base (the tide), and `WaveScale` to raise
the swell (the storm): the drawn surface follows smoothly between ticks, the physics at the next pre-step. Waves whose
steepness would fold the surface are scaled down together (`WATER_WAVES_TOO_STEEP` reports it).

```luau
--!strict
export type Tide = ScriptInstance & {
	Range: number,
	Period: number,
}

local Tide = {}

Tide.Fields = {
	Range = Field.Number(1.5, { Min = 0 }),
	Period = Field.Number(600, { Min = 1 }),
}

function Tide.OnFixedUpdate(self: Tide, dt: number)
	-- The base rises and falls by Range metres over Period seconds of world time.
	local phase = (Time.GetTime() % self.Period) / self.Period
	local height = self.Range * math.sin(phase * 2 * math.pi)
	self.Entity.Transform.Translation = vector.create(self.Entity.Transform.Translation.x, height, self.Entity.Transform.Translation.z)
end

return Script.Define("Tide", Tide)
```

## Floating

A `Buoyancy` on an entity with a Dynamic `RigidBody` floats its body: each solid collider is submerged against the water
plane under its centre at the tick's world time, through Jolt's exact submerged volume, with the water's `Density` and
the component's drags. `BuoyancyScale` 1 is physical (a body floats at the draft its mass and volume give); `Water` names
the water to float on (a `Rectangle` only within its volume), null takes the water that holds each collider. Give a boat
four pontoon colliders on child entities to make it pitch and roll on a swell shorter than its hull; one hull collider
floats on the plane under its centre. A buoyant body on calm, unchanged water may sleep; a tide or a storm wakes it.
`physics.bodyInfo` (through `engine_call`) reports its `buoyancy {water, submergedVolume, totalVolume}`.

A custom floater (a duck that bobs at a point, a buoy with a rope) can push itself instead, with `Physics.SampleWater`
and `RigidBody:AddForceAtPosition` per float point in `OnFixedUpdate`:

```luau
--!strict
export type Floater = ScriptInstance & {
	Lift: number,
	Points: { vector },
}

local Floater = {}

Floater.Fields = {
	Lift = Field.Number(2000, { Min = 0 }),
}

function Floater.OnCreate(self: Floater)
	self.Points = { vector.create(-1, 0, -1), vector.create(1, 0, -1), vector.create(-1, 0, 1), vector.create(1, 0, 1) }
end

function Floater.OnFixedUpdate(self: Floater, dt: number)
	local body = self.Entity.RigidBody
	if body == nil then
		return
	end
	for _, offset in self.Points do
		local point = self.Entity.Transform:TransformPoint(offset)
		local sample = Physics.SampleWater(point)
		if sample ~= nil and sample.Depth > 0 then
			-- A spring toward the surface, capped at a metre of depth.
			body:AddForceAtPosition(sample.Normal * self.Lift * math.min(sample.Depth, 1), point)
		end
	end
end

return Script.Define("Floater", Floater)
```

## Wading and swimming

A character reads the depth under its feet to switch between walking, wading and swimming (swimming itself is
`CharacterController:MoveFree`, M48):

```luau
--!strict
export type Wader = ScriptInstance & {
	Mode: string,
}

local Wader = {}

function Wader.OnCreate(self: Wader)
	self.Mode = "Walk"
end

function Wader.OnFixedUpdate(self: Wader, dt: number)
	local feet = self.Entity.Transform.WorldPosition
	local sample = Physics.SampleWater(feet)
	local depth = if sample ~= nil then sample.Depth else 0
	self.Mode = if depth > 1.2 then "Swim" elseif depth > 0.3 then "Wade" else "Walk"
end

return Script.Define("Wader", Wader)
```
