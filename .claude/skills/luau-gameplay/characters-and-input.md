# Characters, Dynamic bodies and input

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): how a `CharacterController` meets Dynamic bodies (M34, World Tree's brief 11), triggers as buttons and aiming in the fixed step (World Tree's brief 10). The signatures are in `reference/ScriptAPI` (sections `CharacterController`, `Camera` and `Input`); the rules are Architecture §9.6's and §4.3's.

## Characters and Dynamic bodies

- **A character pushes, it is not a wall.** A character pushes the Dynamic bodies its move runs into with at most `Mass` × 9.81 N (the force of its weight; there is no separate push-strength field), so an 80 kg character kicks 0.35 kg debris aside at about its own speed and leans into a heavy crate. Its inner body, the kinematic capsule that raycasts, overlaps and triggers see, never takes part in the solver's contacts: a Dynamic body moving into a character is not stopped by it, it passes through the capsule. A projectile that must hit a character is a trigger or a cast (`Physics.SphereCast`, `Physics.Raycast`), which see the inner body; a rigid body thrown at a character will not bounce off it.
- **Light bodies never move a character.** A Dynamic body whose mass is below the character's `Mass` cannot push it or carry it up its own slopes and edges, gives it none of its velocity when the character stands on it (so a shard under the character's feet never launches it), is never a stair (the character pushes a light box below its `StepHeight` instead of climbing it) and carries at most its own weight from the character, so it is never driven through the floor. A Dynamic body at least as heavy as the character pushes it, carries the character standing on it (up and down, and along with `InheritGroundVelocity`), is climbed like a step and takes the character's full weight.
- **Tune it with `Mass`.** `Mass` decides both how hard the character pushes and which bodies are light for it: raise it for a character that should shoulder through heavier props, lower it for one that should climb crates rather than push them. Debris that a character must walk through without climbing stays below the character's mass.
- **Collision events** between a character and a Dynamic body come from the character's own contacts: `OnCollisionEnter` reaches both when the character's move runs into the body or it stands on it.
- **Layers.** Put debris on a layer of its own and the characters' layer–debris pair in the collision matrix (`Physics.Collisions`); the pushing above needs the pair, and a character on a layer that does not collide with the debris walks through it untouched.

## Triggers as buttons

A Button action binds a trigger like any button: `{"Attack": {"Type": "Button", "Bindings": ["Gamepad.RightTrigger", "Mouse.Left"]}}`. The trigger is down once it is pulled to 0.5 and up again only when it falls below 0.4, so a trigger held near the threshold never chatters; `Input.IsActionDown`, `IsActionPressed` and `IsActionReleased` work as for a button, and an Axis action's `Positive` and `Negative` take a trigger the same way. A suite pulls it with `Test.InjectGamepad(0, "RightTrigger", 0.6)` and lets go with a value below 0.4. A stick (`Gamepad.LeftX` and the like) is no button: it belongs in an Axis action's `Gamepad`.

`project.setSettings` checks the actions of a patch that writes `Input` as `play.start` does and refuses one it would refuse, each problem located under `/patch` (an unknown binding, a stick in a button list, a field the action's type does not have); nothing is written then. `project.validate` reports such an action a project already holds as `INPUT_INVALID_ACTION` (`skills/game-debugging/validation-codes`).

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
