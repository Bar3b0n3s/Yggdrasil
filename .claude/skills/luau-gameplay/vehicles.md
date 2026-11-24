# Vehicles in scripts

Wheeled vehicles (M44, Architecture §9.11) drive a Dynamic body on wheels through Jolt's vehicle: a `WheeledVehicle` on
the body's entity (the chassis, with its `RigidBody` and a solid collider) and a `VehicleWheel` on each wheel entity
below it. A wheel is a cast, not a body: it carries no collider, and its entity is where the engine places the wheel's
mesh every physics step (its suspension, steer and spin), so a `MeshRenderer` on it rides along. Add a `MassProperties`
beside the chassis's `RigidBody` to lower its centre of mass (a negative `CenterOfMassOffset.y`), or a brisk turn rolls
it over.

## Making one

- **The wheels.** Author each wheel entity where its centre hangs at full droop, its local X along the axle (its rotation
  gives camber and toe). Its suspension runs along the chassis's -Y from a point `SuspensionMaxLength` above that centre,
  and it steers about the chassis's +Y up to `MaxSteerAngle` degrees (0 for a wheel that does not steer). `Radius` and
  `Width` are metres, never scaled; `SpringFrequency` (Hz) and `SpringDamping` set the ride; `MaxBrakeTorque` and
  `MaxHandBrakeTorque` (0 for a wheel without a hand brake) the brakes; `LongitudinalFriction` and `LateralFriction` the
  tyre's grip by slip, which the ground's friction scales (`sqrt(tyre × ground)`: a `ColliderSurface`'s, else its body's).
- **The drivetrain.** `MaxTorque` (N m) by `TorqueCurve` (X is the RPM over `MaxRpm`), `MinRpm` and `MaxRpm`;
  `Transmission` `Auto` (shifts at `ShiftUpRpm` and `ShiftDownRpm`) or `Manual` (`Gear` and `Clutch`); `GearRatios` and
  `ReverseGearRatios`. `Differentials` name the driven wheels in pairs, each with its `Ratio` (the final drive) and its
  share of the engine's torque, `EngineTorqueRatio`: the shares of the differentials that name a wheel sum to 1 (a
  four-wheel drive: two differentials at 0.5). `AntiRollBars` join a left and a right wheel. A vehicle without a
  differential that names a wheel is `PHYSICS_VEHICLE_INVALID` "differential".
- **The cast.** `WheelCast` `Cylinder` (each wheel's own radius and width, the wide tyre), `Ray` (one ray straight down
  each suspension) or `Sphere` (one sphere, the vehicle's smallest wheel `Radius`, for every wheel).
- **A trailer** drives no wheel: give it one differential naming two of its wheels, in `Manual` at `Gear` 0, so its
  gear ratio is 0 and no torque reaches the wheels; tow it with a `DistanceJoint` (`skills/luau-gameplay/joints`).

`PHYSICS_VEHICLE_INVALID` names the first thing wrong (`project.validate` before play, a script error once in play): no
Dynamic body, no wheel, a wheel under another `RigidBody`, a collider on a wheel, a suspension, engine, transmission,
differential, bar or curve rule. Such a vehicle is not created, and its chassis falls as a plain body.

## Driving and reading

`Throttle` (-1 to 1), `Steering` (-1 full left lock to 1 full right lock), `Brake` and `HandBrake` (0 to 1) are the
driver's input, with `Gear` and `Clutch` in `Manual`. Write them from `OnFixedUpdate` (or a task, or a test case): the
next physics pre-step applies them, and a sleeping vehicle wakes for them. `Auto` reverses for a negative `Throttle`;
`Manual` drives by a `Throttle` from 0 to 1 in its `Gear`'s direction. Simulate runs no script, so a vehicle keeps its
authored input there. A driver on W, S, A, D and Space:

```luau
--!strict
export type Driver = ScriptInstance & {
	TopSpeed: number,
}

local Driver = {}

local function axis(positive: Key, negative: Key): number
	return (if Input.IsKeyDown(positive) then 1 else 0) - (if Input.IsKeyDown(negative) then 1 else 0)
end

function Driver.OnCreate(self: Driver)
	self.TopSpeed = 0
end

function Driver.OnFixedUpdate(self: Driver, dt: number)
	local vehicle = self.Entity.WheeledVehicle
	if vehicle == nil then
		return
	end
	vehicle.Throttle = axis("W", "S")
	vehicle.Steering = axis("D", "A")
	vehicle.HandBrake = if Input.IsKeyDown("Space") then 1 else 0
	self.TopSpeed = math.max(self.TopSpeed, vehicle:GetSpeed())
end

return Script.Define("Driver", Driver)
```

The reads report the vehicle after the last physics step, in every callback: `WheeledVehicle:GetSpeed()` (m/s along
the chassis's forward, local -Z, negative backward), `GetEngineRpm()`, `GetGear()` (negative reverse, 0 neutral) and
`IsShifting()`; per wheel, `VehicleWheel:IsTouching()`, `GetContact()` (the point, the ground's normal and the collider
entity, three nils in the air: `Physics.GetSurface` reads that collider's friction for dust or skid sounds),
`GetSuspensionLength()` (m), `GetSuspensionForce()` (N), `GetRotation()` (degrees, 0 to 360), `GetSteerAngle()`
(degrees, positive right), `GetAngularVelocity()` (degrees per second, positive rolling forward) and `GetSlip()` (the
slip ratio and the slip angle in degrees). They raise "entity has no vehicle in force" for a vehicle that is refused,
disabled or not made yet, and "physics is unavailable" outside a play session. `physics.vehicleInfo` reports the same
to an agent (`skills/game-debugging`).

## Tuning against the numbers

- **Top speed.** Jolt keeps the engine's torque at `MaxRpm` (the curve's value at X = 1), and a slipping clutch passes it
  on, so with the default curve (0.8 at 1) a vehicle without drag runs past its last gear's speed. A curve that falls to 0
  at `MaxRpm` (a rev limiter, ending in `{X = 0.9, Y = 1}, {X = 1, Y = 0}`) holds the top speed at `MaxRpm × 2π / 60 / (top gear
  ratio × differential ratio) × Radius`.
- **Ride height.** Each spring of frequency f carries its share of the weight with the stiffness `m_eff (2π f)²`, m_eff
  the vehicle's effective mass along the suspension at the wheel (`1 / (1/m + (r × n) I⁻¹ (r × n))`, r from the centre of
  mass, so a long or narrow chassis sits lower than its mass alone says). The rest length is `SuspensionMaxLength` less
  the share of the weight over that stiffness. `Ray` casts meet it to the millimetre; a `Cylinder`'s deepest contact lies
  off the wheel's centre plane, which shares the weight a few per cent unevenly between left and right.
- **Rolling over.** A full-lock turn rolls the chassis when its centre of mass stands high over a narrow track: lower it
  with `MassProperties` (below the axles is allowed), widen the track, or stiffen the `AntiRollBars`. `MaxPitchRollAngle`
  (degrees, 180 off) holds the chassis from tilting further, for an arcade vehicle.
- **Grip by surface.** A wheel grips by its tyre curves times its ground's friction: a `ColliderSurface` strip of
  `Friction` 0.3 is ice beside a rough 1.0, and a terrain grips with its `TerrainCollider`'s `Friction`.

## Things to know

- **Disable a wheel to take it off**, don't destroy it: a destroyed wheel that a differential or bar names makes the
  vehicle invalid, while a disabled wheel is left out (a disabled driven pair that leaves the vehicle undriven is reported,
  naming the wheels).
- A write to a wheel's field, the wheel set, the cast or the bars' wheels makes the vehicle again on its chassis, keeping
  its speed, revs, gear and wheels' spin; writing the driver input every tick never does. Each wheel's authored pose is
  kept for the session, so a vehicle made again, a wheel moved under another child of the chassis, or a component removed
  and added back finds its wheels where they were authored.
- Scale a vehicle before play, or instantiate it at its new scale: scaling it in play scales its chassis, but its wheels
  keep hanging where they hung at the scale it was made with. A wheel moved under another vehicle hangs there by the
  local pose it was first placed with.
- A parked vehicle's wheels keep turning by their last spin, slowed by their `AngularDamping`, while its body sleeps: the
  wheel meshes creep briefly. Brake to stop them, or raise `AngularDamping`.
- A wheel turning more than half a turn per tick (about 188 rad/s at 60 Hz) aliases on screen: the physics is exact.
- Wheels never meet characters (their casts use the chassis's layer); a character pushes the chassis.
