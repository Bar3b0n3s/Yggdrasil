# Characters, Dynamic bodies and input

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): how a `CharacterController` meets Dynamic bodies (M34, World Tree's brief 11), what it stands on and the surfaces that carry it, Kinematic bodies that push it and the squeeze (`IsSqueezed`, `GetSqueezingBody`) (M36, World Tree's briefs 15 and 16), triggers as buttons and aiming in the fixed step (World Tree's brief 10). The signatures are in `reference/ScriptAPI` (sections `CharacterController`, `Physics`, `Camera` and `Input`); the rules are Architecture §9.2's, §9.6's and §4.3's.

## Characters and Dynamic bodies

- **A character pushes, it is not a wall.** A character pushes the Dynamic bodies its move runs into with at most `Mass` × 9.81 N (the force of its weight; there is no separate push-strength field), so an 80 kg character kicks 0.35 kg debris aside at about its own speed and leans into a heavy crate. Its inner body, the kinematic capsule that raycasts, overlaps and triggers see, never takes part in the solver's contacts, so what stops a Dynamic body moving into a character is the character's own push: a light body thrown at a standing character is stopped by it, but passes into the capsule when it outruns the character's look-ahead (0.1 m) within one tick or something drives it harder than the push, and a body heavier than the character shoves it aside, overlapping it only when the character is pinned against a wall or under the body. A projectile that must hit a character is a trigger or a cast (`Physics.SphereCast`, `Physics.Raycast`), which see the inner body.
- **Light bodies never move a character.** A Dynamic body whose mass is below the character's `Mass` cannot push it or carry it up its own slopes and edges (a character already standing on one walks across it, up a tilted plank too), gives it none of its velocity when the character stands on it (so a shard under the character's feet never launches it, and a light platform never lifts or carries a character: make a moving platform Kinematic, or heavier than its riders), is never a stair (the character pushes a light box below its `StepHeight` instead of climbing it) and carries at most its own weight from the character, so it is never driven through the floor. A Dynamic body at least as heavy as the character pushes it, carries the character standing on it (up and down, and along with `InheritGroundVelocity`), is climbed like a step and takes the character's full weight.
- **Tune it with `Mass`.** `Mass` decides both how hard the character pushes and which bodies are light for it: raise it for a character that should shoulder through heavier props, lower it for one that should climb crates rather than push them. Debris that a character must walk through without climbing stays below the character's mass.
- **Collision events** between a character and a Dynamic body come from the character's own contacts: `OnCollisionEnter` reaches both when the character's move runs into the body or it stands on it.
- **Layers.** Put debris on a layer of its own and the characters' layer–debris pair in the collision matrix (`Physics.Collisions`); the pushing above needs the pair, and a character on a layer that does not collide with the debris walks through it untouched.

## What a character stands on: surfaces and belts

- **The ground collider.** `CharacterController:GetGroundCollider()` is the collider entity the character's last update stood on (a piece of a level compound, another character's entity), or nil in the air; it comes from the update itself, so it costs no ray. `Physics.GetSurface(collider)` gives the values that collider's contacts use, `{Friction, Restitution, SurfaceVelocity}`: its `ColliderSurface`'s, the velocity in world space, or its body's friction and restitution with no velocity; it takes nil and returns nil, so `Physics.GetSurface(controller:GetGroundCollider())` is nil in the air. Together they replace a downward ray and a tag for footing.
- **Mixed floors in one compound.** Give each piece of a floor its own `ColliderSurface` (ice at friction 0.02, marble keeping the level's 0.5): a piece's contacts take its surface, so debris slides on the ice and stops on the marble beside it, with one Static `/Level` and no seam (`skills/game-building/prefabs-and-levels`). A surface applies to the solid colliders on its own entity only, never to its children's, a trigger or a character's capsule (`PHYSICS_SURFACE_UNUSED`).
- **Characters do not slide by themselves.** A character has no friction: the engine never slides it on ice. A game that wants slipping reads the ground's friction and changes the velocity it gives `Move`.
- **Belts.** A `ColliderSurface`'s `SurfaceVelocity` (m/s along the entity's own axes, so a turned belt carries along its length) carries what lies on it through friction, without moving the collider, and carries a grounded character whatever its `InheritGroundVelocity` says, added to what its `Move` asks for. Only its part along the surface carries anything (a character's projected onto its ground plane), so a velocity pointing out of the surface neither lifts nor presses what lies on it. Write it from a behaviour to stop, reverse or speed up a belt: a write takes effect at the next physics step (the same tick's from `OnFixedUpdate`), never rebuilds the body, and wakes what sleeps on the belt only when the value changes, so writing the same speed every tick costs nothing.

```luau path=Assets/Scripts/Footing.luau
--!strict
export type Footing = ScriptInstance & {
	Slipping: boolean,
}

local Footing = {}

function Footing.OnCreate(self: Footing)
	self.Slipping = false
end

function Footing.OnFixedUpdate(self: Footing, dt: number)
	local controller = self.Entity.CharacterController
	if not controller then
		return
	end
	local ground = controller:GetGroundCollider()
	local surface = if ground then Physics.GetSurface(ground) else nil
	-- Ice: the game slides its own character.
	self.Slipping = surface ~= nil and surface.Friction < 0.1
end

return Script.Define("Footing", Footing)
```

```luau path=Assets/Scripts/Conveyor.luau
--!strict
export type Conveyor = ScriptInstance & {
	Speed: number,
	Running: boolean,
}

local Conveyor = {}

Conveyor.Fields = {
	Speed = Field.Number(2.5),
	Running = Field.Bool(true),
}

function Conveyor.OnFixedUpdate(self: Conveyor, dt: number)
	local surface = self.Entity.ColliderSurface
	if surface then
		surface.SurfaceVelocity = vector.create(if self.Running then self.Speed else 0, 0, 0)
	end
end

return Script.Define("Conveyor", Conveyor)
```

## Pushes and the squeeze

- **Kinematic bodies push characters.** A piston, a press or a door moved with `RigidBody:MoveKinematic` (or a Dynamic body at least as heavy as the character) pushes a character it moves into at its own velocity, up to about 6 m/s at 60 Hz without ever entering the capsule; a faster one overlaps it for one tick first, and one moving more than about 0.4 m a tick can pass through a 0.3 m character. Keep machines below that, or make the fast part a trigger that deals the hit. A pushed character's `GetVelocity()` stays what its `Move` asked for; read how far it was pushed from its position. The pushing part needs a Kinematic `RigidBody` of its own moved with `MoveKinematic`: a collider moved under its body pushes nothing. Within the fixed step, such a body's Transform holds its target as soon as `MoveKinematic` is called, a step ahead of the body; to compare it with where a character is, read where the body is with `Physics.GetBodyBounds`.
- **The squeeze.** A character the body cannot move out of the way (pinned under a press, against a wall by a piston) is squeezed: `CharacterController:IsSqueezed()` is true from the next fixed update, and `GetSqueezingBody()` names the body's entity. The engine does nothing else: there is no callback, no escape and no damage, and under a flat face it never shoves the character sideways. Read it in `OnFixedUpdate` and crush or shove the character yourself. A resting body (a hammer the belt carries the character into), the character's own walk into a wall and a press still above its head are never a squeeze.

```luau path=Assets/Scripts/Crushable.luau
--!strict
export type Crushable = ScriptInstance & {
	Crushed: boolean,
}

local Crushable = {}

function Crushable.OnCreate(self: Crushable)
	self.Crushed = false
end

function Crushable.OnFixedUpdate(self: Crushable, dt: number)
	local controller = self.Entity.CharacterController
	if not controller or self.Crushed or not controller:IsSqueezed() then
		return
	end
	self.Crushed = true
	local press = controller:GetSqueezingBody()
	Log.Info(`{self.Entity.Name} was crushed by {if press then press.Name else "something"}`)
end

return Script.Define("Crushable", Crushable)
```

## Triggers as buttons

A Button action binds a trigger like any button: `{"Attack": {"Type": "Button", "Bindings": ["Gamepad.RightTrigger", "Mouse.Left"]}}`. The trigger is down once it is pulled to 0.5 and up again only when it falls below 0.4, so a trigger held near the threshold never chatters; `Input.IsActionDown`, `IsActionPressed` and `IsActionReleased` work as for a button, and an Axis action's `Positive` and `Negative` take a trigger the same way. A suite pulls it with `Test.InjectGamepad(0, "RightTrigger", 0.6)` and lets go with a value below 0.4. A stick (`Gamepad.LeftX` and the like) is no button: it belongs in an Axis action's `Gamepad`.

`project.setSettings` checks the actions of a patch that writes `Input` as `play.start` does and refuses one it would refuse, each problem located under `/patch` (an unknown binding, a stick in a button list, a field the action's type does not have); nothing is written then. (The editor's own Project Settings panel commits such a patch, so a binding added empty can be filled in, and shows each problem beside its binding until it is fixed.) `project.validate` reports such an action a project already holds as `INPUT_INVALID_ACTION` (`skills/game-debugging/validation-codes`).

## Aiming in the fixed step

Gameplay never reads the camera's pose in the fixed step (the camera moves in the frame phase), and `Camera:ScreenToWorldRay` projects from the camera's rendered, interpolated pose. To aim at the cursor in `OnFixedUpdate` (a ground-targeted ability, a mouse-aimed attack), keep the camera rig's pose in the fixed step and give it to `Camera:ScreenToWorldRayFrom(x, y, position, rotation)`, which builds the same ray from that pose; the frame phase then places the rendered camera from the same rig state, as a follow camera does. The view size is the session's, fixed in every suite, replay and recording, so the aim is deterministic:

```luau path=Assets/Scripts/GroundAim.luau
--!strict
export type GroundAim = ScriptInstance & {
	Yaw: number,
	Target: vector?,
}

local GroundAim = {}

function GroundAim.OnCreate(self: GroundAim)
	self.Yaw = 0
	self.Target = nil
end

function GroundAim.OnFixedUpdate(self: GroundAim, dt: number)
	self.Yaw += Input.GetAxis("TurnCamera") * 90 * dt
	-- The rig's pose, kept here: 14 m from the player, looking down at 45 degrees.
	local rotation = Quat.FromEuler(-45, self.Yaw, 0)
	local position = self.Entity.Transform.WorldPosition + rotation:Rotate(vector.create(0, 0, 14))
	local camera = Scene.GetPrimaryCamera()
	local lens = camera and camera.Camera
	if not lens then
		return
	end
	local cursor = Input.GetMousePosition()
	local size = Application.GetWindowSize()
	if cursor.x < 0 or cursor.y < 0 or cursor.x >= size.x or cursor.y >= size.y then
		return
	end
	local origin, direction = lens:ScreenToWorldRayFrom(cursor.x, cursor.y, position, rotation)
	local hit = Physics.Raycast(origin, direction, 100)
	self.Target = if hit then hit.Point else nil
end

return Script.Define("GroundAim", GroundAim)
```
