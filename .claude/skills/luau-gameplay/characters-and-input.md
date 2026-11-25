# Characters, Dynamic bodies and input

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): how a `CharacterController` meets Dynamic bodies (M34, World Tree's brief 11; M48's light bodies that support it, Skerries' brief S19), swimming, climbing and gliding with free moves (M48, Skerries' brief S17), what it stands on and the surfaces that carry it, Kinematic bodies that push it and the squeeze (`IsSqueezed`, `GetSqueezingBody`) (M36, World Tree's briefs 15 and 16), triggers as buttons and aiming in the fixed step (World Tree's brief 10). The signatures are in `reference/ScriptAPI` (sections `CharacterController`, `Physics`, `Camera` and `Input`); the rules are Architecture §9.2's, §9.6's and §4.3's.

## Characters and Dynamic bodies

- **A character pushes, it is not a wall.** A character pushes the Dynamic bodies its move runs into with at most `Mass` × 9.81 N (the force of its weight; there is no separate push-strength field), so an 80 kg character kicks 0.35 kg debris aside at about its own speed and leans into a heavy crate. Its inner body, the kinematic capsule that raycasts, overlaps and triggers see, never takes part in the solver's contacts, so what stops a Dynamic body moving into a character is the character's own push: a light body thrown at a standing character is stopped by it, but passes into the capsule when it outruns the character's look-ahead (0.1 m) within one tick or something drives it harder than the push, and a body heavier than the character shoves it aside, overlapping it only when the character is pinned against a wall or under the body. A projectile that must hit a character is a trigger or a cast (`Physics.SphereCast`, `Physics.Raycast`), which see the inner body.
- **Light bodies hold a character up, and never move it otherwise.** A Dynamic body whose mass is below the character's `Mass` supports a character standing on it (M48): its walkable surface holds the character on top, so a rope bridge, a raft or a jetty of light planks holds it up, rising and sinking with the deck without the character sinking into it, and the deck still gives under it. It gives the character none of its velocity (so a shard under its feet never launches it, and a light platform lifts but never carries a character along: make a moving platform Kinematic, or heavier than its riders, for that), cannot push it with its steep faces or when it is not what the character stands on, does not carry it up its own slopes and edges from the floor (a character already on a light body walks across it, up a tilted plank too, and onto the next light plank of a bridge), is never a stair (the character pushes a light box below its `StepHeight` instead of climbing it) and carries at most its own weight from the character, so it is never driven through the floor. A light body the character steps onto passes into the capsule until the next update stands on it, which then lifts it onto the top. A Dynamic body at least as heavy as the character pushes it, carries the character standing on it (up and down, and along with `InheritGroundVelocity`), is climbed like a step and takes the character's full weight.
- **Tune it with `Mass`.** `Mass` decides both how hard the character pushes and which bodies are light for it: raise it for a character that should shoulder through heavier props, lower it for one that should climb crates rather than push them. Debris that a character must walk through without climbing stays below the character's mass.
- **Collision events** between a character and a Dynamic body come from the character's own contacts: `OnCollisionEnter` reaches both when the character's move runs into the body or it stands on it.
- **Layers.** Put debris on a layer of its own and the characters' layer–debris pair in the collision matrix (`Physics.Collisions`); the pushing above needs the pair, and a character on a layer that does not collide with the debris walks through it untouched.

## Off the ground: swimming, climbing and gliding

- **`CharacterController:MoveFree(velocity)`** (M48) is `Move` for a character off the ground: in the air the next fixed update takes the whole velocity, its vertical part included, with no gravity and no floor sticking; while the character is grounded as the update begins (on walkable ground and not leaving it faster than 0.1 m/s) it is exactly `Move`, so a swimmer that reaches the bottom stands, and an upward part leaves the ground as a jump does. `IsGrounded()` reads the ground after the last update, so in the update in which a free mover lands, or the one after a jump, it can differ from what decides the next `MoveFree`; `physics.bodyInfo`'s `lastMove` says which the last update was. Call it in every `OnFixedUpdate` you want it, as `Move`: the last `Move` or `MoveFree` of a tick wins, also in a tick whose field write (`Height`, `Radius`) creates the character again, and a character that stops calling it in the air falls from its last velocity, which a `Move` continues with gravity (a swimmer surging out of the water keeps rising briefly). A new character (at the scene's start, a spawn, or a `Height` or `Radius` write that creates it again) is not grounded in its first update, so a `MoveFree` there is free even on the floor, and a ledge hold keeps it where it was created. A climber that holds while it is not grounded starts off its ladder, or calls `Move` in its first tick.
- **Swim:** the target height is the surface less the depth to hold; ask for the vertical velocity that reaches it in one step, `(target - y) / dt`, clamped, with the horizontal swim beside it. Free moves track it exactly, so the engine adds no oscillation. Where the bottom is higher than the target (the shallows), the character stands and walks with `Move`.
- **Climb:** a trigger marks the ladder; on it, `MoveFree(intoWall * 0.5 + up * climbSpeed)` presses the character to the wall and rises or comes down; at the top a forward `Move` steps off onto the floor. **Ledge hold:** `MoveFree(vector.zero)`. **Glide:** `MoveFree` with the sink rate you choose.
- A light Dynamic body (below the character's `Mass`) holds a character standing on it, so a raft, a rope bridge or a jetty of light planks holds it up, pressed with at most the body's own weight: a 25 kg raft under a 70 kg character is pressed as by 25 kg, not 70 ("Characters and Dynamic bodies" above).

```luau path=Assets/Scripts/Swimmer.luau
--!strict
export type Swimmer = ScriptInstance & {
	Depth: number,
	Speed: number,
}

local Swimmer = {}

Swimmer.Fields = {
	Depth = Field.Number(1.3, { Min = 0 }),
	Speed = Field.Number(4, { Min = 0 }),
}

-- The sea's surface: a swell about y 0 here, the water's height where the game has water.
local function surface(ticks: number): number
	return 0.15 * math.sin(ticks * Time.GetFixedDeltaTime() * 0.9)
end

function Swimmer.OnFixedUpdate(self: Swimmer, dt: number)
	local controller = self.Entity.CharacterController
	if not controller then
		return
	end
	local horizontal = vector.create(Input.GetAxis("MoveX"), 0, -Input.GetAxis("MoveY")) * self.Speed
	local target = surface(Time.GetSessionTick() + 1) - self.Depth
	local y = self.Entity.Transform.WorldPosition.y
	if controller:IsGrounded() and y >= target then
		-- The bottom is higher than the depth held: stand and walk.
		controller:Move(horizontal)
		return
	end
	local rise = math.clamp((target - y) / dt, -3, 3)
	controller:MoveFree(horizontal + vector.create(0, rise, 0))
end

return Script.Define("Swimmer", Swimmer)
```

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
