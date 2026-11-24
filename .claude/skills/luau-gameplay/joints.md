# Joints in scripts

Joints (M37, Architecture §9.10) join a body to another body or to the world: `HingeJoint` (a door, a trap door, a
wheel), `SliderJoint` (a piston, a rail), `DistanceJoint` (a rope, a chain link, a rod) and `FixedJoint` (a weld that
breaks). Each is a component on the body's entity, or on a child of it; `ConnectedBody` names the other end (null: the
world). One joint per entity: put a second joint of the same body on a child entity.

## Making one

Add the component as any other (`entity.create`, `entity.update`, or `Entity:AddComponent` in a script). What the
fields mean:
- **Where.** `Anchor` is the joint's point in its entity's space (metres; the entity's scale applies); `Axis` the
  hinge's or slider's axis in the same space. The pose the bodies have when the joint is first made is the hinge's angle
  0 and the slider's position 0, so author the door closed and the piston retracted.
- **Limits.** `UseLimits` with `LowerAngle`/`UpperAngle` (degrees, −180 to 0 and 0 to 180) or `LowerLimit`/`UpperLimit`
  (metres). Equal limits make the joint invalid (`PHYSICS_JOINT_INVALID`): use a `FixedJoint` for a joint that does not
  move. A `DistanceJoint` keeps the distance between `Anchor` and `ConnectedAnchor` (in `ConnectedBody`'s space, world
  space when it is null) between `MinDistance` and `MaxDistance`; equal values make a rod, and `Frequency` and `Damping`
  soften the limits into an elastic rope.
- **Motors.** `Motor` `Velocity` drives at `MotorTargetVelocity` (deg/s or m/s); `Position` drives to `MotorTargetAngle`
  or `MotorTargetPosition` through a spring of `MotorFrequency` (Hz) and `MotorDamping`; both apply at most
  `MotorMaxTorque` (N m) or `MotorMaxForce` (N). With the motor `Off`, `MaxFrictionTorque`/`MaxFrictionForce` resist the
  motion. A target is clamped to the limits while `UseLimits` holds.
- **Collisions.** While `CollideConnected` is false (the default) the two joined bodies never collide with each other,
  and no collision callback runs between them. The connected end is a whole body: a `ConnectedBody` whose colliders an
  ancestor gathers into one static body (a level root's) keeps the joined body apart from every collider of that body,
  the floor included; give the anchor its own `RigidBody` (Static) or set `CollideConnected`.
- **Breaking.** `BreakForce` (N) and `BreakTorque` (N m; not on a `DistanceJoint`), 0 never breaking.

Jolt solves a joint only while one of its bodies is Dynamic and awake: a joint between two Static or Kinematic bodies
does nothing. A trap door at least as heavy as the character takes the character's whole weight, so its motor torque
must carry it (its `CharacterController.Mass` times gravity, at the lever arm). A door lighter than the character
still supports a character standing on it and lifts it as it swings up, but takes at most its own weight from it, so
its motor need not carry the character's (`skills/luau-gameplay/characters-and-input`).

## Driving and reading

Every field is read and written through the proxy. A write takes effect at the next physics pre-step: the same tick's
from `OnFixedUpdate`, a task or a test case, the next tick's from `OnUpdate` or a physics callback. Changing a motor,
a target, a limit or a threshold every tick changes the live joint and wakes its bodies, so a trap door opens and
closes by its motor:

```luau
--!strict
export type TrapDoor = ScriptInstance & {
	Open: boolean,
}

local TrapDoor = {}

function TrapDoor.OnCreate(self: TrapDoor)
	self.Open = false
end

function TrapDoor.OnFixedUpdate(self: TrapDoor, dt: number)
	local hinge = self.Entity.HingeJoint
	if hinge == nil then
		return
	end
	-- Drop what stands on the door when it is told to, and close it again.
	hinge.MotorTargetAngle = if self.Open then -80 else 0
	if self.Open and hinge:GetAngle() < -75 then
		self.Open = false
	end
end

return Script.Define("TrapDoor", TrapDoor)
```

The reads report the joint after the last physics step (and a teleport since), in every callback:
`HingeJoint:GetAngle()` (degrees from the authored pose), `SliderJoint:GetPosition()` (metres along `Axis`),
`DistanceJoint:GetDistance()` (metres between the anchors), and each type's `GetForce()` (N) and, but on a
`DistanceJoint`, `GetTorque()` (N m), the last collision step's force and torque as the break test measures them. They
raise "entity has no joint in force" for a joint that is refused, broken, disabled or not made yet, and "physics is
unavailable" outside a play session.

## Breaking and mending

After each physics step, a joint whose force passes `BreakForce` or whose torque passes `BreakTorque` breaks: it is
removed, `Broken` becomes true and `OnJointBreak(self, force, torque)` runs on the behaviours of the joint's entity,
after that step's collision and trigger callbacks. Writing `Broken = false` mends it where it was first made; the solver
pulls the bodies back together over the next ticks, not at once. Writing `Broken = true` removes a joint without the
callback.

```luau
--!strict
export type Weld = ScriptInstance & {
	BrokeAt: number,
}

local Weld = {}

function Weld.OnCreate(self: Weld)
	self.BrokeAt = -1
end

function Weld.OnJointBreak(self: Weld, force: number, torque: number)
	self.BrokeAt = Time.GetTick()
	Log.Info(string.format("the weld broke under %.0f N and %.0f N m", force, torque))
end

return Script.Define("Weld", Weld)
```

The force is measured per collision step, so set a threshold well away from the load the joint carries at rest (read
it with `GetForce` first): a joint near its threshold breaks on a step's peak, and at a `Simulation.FixedHz` below 60 a
tick holds several collision steps. A script that wants damage accumulated over several hits sums `GetForce` itself.

## Things to know

- Destroying a joint's connected entity leaves the joint naming nothing: it reports `PHYSICS_JOINT_INVALID` once (a
  script error). Clear `ConnectedBody`, remove the joint or destroy the joint's entity first.
- A body created again (its RigidBody changed, its entity switched off and on) gets its joints back where they were
  made, so a door keeps its zero and its limits. A change of `ConnectedBody`, `Anchor` or `Axis`, or moving the joint's
  entity to another body, makes the joint again from the poses of that moment; a `DistanceJoint`'s new `Anchor` or
  `ConnectedAnchor` applies at the next physics step.
- Long chains of very different masses stretch at the solver's default iterations; keep link masses alike.
