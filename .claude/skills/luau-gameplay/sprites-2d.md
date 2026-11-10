# Sprites and 2D physics in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): the script surface of sprites, sprite animation, 2D lights, tilemaps and the 2D physics world. How 2D games are put together: `skills/game-building/sprites-2d`; when they misbehave: `skills/game-debugging/sprites-2d`. The signatures are in `reference/ScriptAPI` (sections `SpriteRenderer`, `SpriteAnimator`, `Light2D`, `TilemapRenderer`, `TilemapCollider`, `RigidBody2D`, `Physics2D`).

**Fields** are reached through the component shortcuts, which can be nil: `self.Entity.SpriteRenderer` (`Frame`, `FlipX`, `FlipY`, `Color`, `SortingLayer`, `OrderInLayer`, `PixelsPerUnit`, `Sheet`, `Texture`, `Lit`, `NormalMap`), `SpriteAnimator` (`Clip`, `Playing`, `Step`, `StepTicks`), `PixelPerfectCamera`, `Light2D` (`Type` `"Point"` or `"Global"`, `Color`, `Intensity`, `Radius`, `Falloff`, `Height`), `TilemapRenderer` (`Tilemap`, `Color`, `PixelsPerUnit`, `SortingLayer`, `OrderInLayer`, `Lit`), `TilemapCollider` (`Tilemap`, `IsTrigger`), `RigidBody2D` and the 2D colliders. Facing and y-sorting are field writes in the fixed step:

```luau path=Assets/Scripts/Facing.luau
--!strict
export type Facing = ScriptInstance & {
	LastX: number,
}

local Facing = {}

function Facing.OnStart(self: Facing)
	self.LastX = self.Entity.Transform.WorldPosition.x
end

function Facing.OnFixedUpdate(self: Facing)
	local sprite = self.Entity.SpriteRenderer
	if not sprite then
		return
	end
	local position = self.Entity.Transform.WorldPosition
	if position.x ~= self.LastX then
		sprite.FlipX = position.x < self.LastX
	end
	self.LastX = position.x
	-- Lower sprites draw over higher ones in the same sorting layer.
	sprite.OrderInLayer = -math.floor(position.y * 16)
end

return Script.Define("Facing", Facing)
```

**Sprites and animation:**
- `SpriteRenderer:SetFrameByName(name)` sets `Frame` to the index of the sheet's frame named `name`; a name the sheet lacks raises NotFound naming the sheet. Frames are identified by index (`Frame = 3`); names only label them.
- `SpriteAnimator:Play(clip)` sets `Clip`, resets `Step` and `StepTicks` and sets `Playing`; `SpriteAnimator:Stop()` clears `Playing` and keeps the frame shown. Clips advance in the SpriteAnimation phase, after `OnFixedUpdate`, tasks and native systems: a clip played in `OnFixedUpdate` shows its first step at that tick, one played in a contact callback or `OnDestroy` from the next. A step shows its frame for exactly its `Ticks` fixed steps, so read `SpriteRenderer.Frame` or `SpriteAnimator.Step` to sync gameplay (an attack's hit frame) instead of counting time.
- A clip the sheet lacks plays nothing (`SPRITE_UNKNOWN_CLIP`); there is no speed field: play another clip for another speed.

**Lights and tilemaps:**
- A `Light2D`'s fields are presentation only: a script may flicker a torch (`Intensity`) or move a light with its entity, and no replay's hash changes. A sprite takes the lights only with `Lit` set; `NormalMap` takes a texture imported as a normal map (`Assets.Load(path)`), or nil for the flat normal.
- A tilemap's cells belong to its `.tilemap` asset, painted with `tilemap.paint` in the editor or from automation: scripts do not change cells. A script shows another map by writing `TilemapRenderer.Tilemap` (a `Field.Asset("Tilemap")` property can hold one), and a `TilemapCollider` whose `Tilemap` is nil follows its renderer's; the collider's shape is rebuilt at the next 2D pre-step, as it is when a script writes the collider's own `Tilemap` or `IsTrigger`. Rays and overlaps that meet the tilemap's boxes report the tilemap's entity, and contacts and triggers with them arrive as `OnCollisionEnter2D` and `OnTriggerEnter2D` with that entity as `other`.

**`RigidBody2D`** methods mirror `RigidBody`'s in the plane: `AddForce(force)`, `AddForceAtPosition(force, position)`, `AddTorque(torque: number)`, `AddImpulse(impulse)`, `AddAngularImpulse(impulse: number)`, `GetLinearVelocity()`, `SetLinearVelocity(velocity)`, `GetAngularVelocity() -> number`, `SetAngularVelocity(velocity: number)`, `MoveKinematic(position, angle)`, `Teleport(position, angle?)`, `IsSleeping()` and `WakeUp()`. A vector's X and Y are used and its Z ignored; a returned vector has Z 0; angles are degrees counter-clockwise about +Z, angular velocities radians per second.

**The `Physics2D` module:** `Raycast(origin, direction, maxDistance, layerMask?) -> RaycastHit?` (the nearest hit), `RaycastAll(...) -> {RaycastHit}` (by distance, then collider UUID), `OverlapCircle(center, radius, layerMask?) -> {Entity}`, `OverlapBox(center, halfExtents, angle?, layerMask?) -> {Entity}` (each overlapping entity once, by UUID; the box turned by `angle` degrees about +Z), `GetGravity() -> vector` and `SetGravity(gravity)`. Masks come from `Physics.LayerMask(...)`, as in 3D; a hit's vectors have Z 0.

**Callbacks:** `OnCollisionEnter2D(self, other: Entity, contact: CollisionContact)`, `OnCollisionExit2D(self, other)`, `OnTriggerEnter2D(self, other)` and `OnTriggerExit2D(self, other)`. They follow the 3D callbacks' rules (held for an instance that has not started, every script slot of the receiving entity in slot order, a destroyed partner readable until the flush) and arrive in canonical order. The 2D world steps after the 3D world: a 2D callback sees both worlds stepped, a 3D callback the 2D world before its step.

**Determinism:** 2D bodies, animators and sorting fields are simulation state that the state hash covers, so drive them only from the fixed step and latched input, as any gameplay. Set `OrderInLayer` from positions rounded to whole numbers (as above), never from wall-clock time or frame counts.
