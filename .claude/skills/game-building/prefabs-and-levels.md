# Prefabs and levels

A reference page of the `game-building` skill (`skills/game-building`), for its step "3. Scenes, assets and prefabs".

## Prefabs

- Prefabs: build an entity, `prefab_create {entity, path, replaceWithInstance?}`, then `prefab_instantiate {prefab, parent?, index?, transform?, name?, components?}`; `prefab_apply` writes an instance's overrides into the prefab and `prefab.revert` (through `engine_call`) drops them. `prefab_create` returns the replacing instance as `instance`, `prefab_instantiate` the new root as `entity` (`{"$ref": "2.entity.id"}` in a batch). Deleting an instance's member (an entity below its root that the prefab made) records a `RemoveEntity` override, so it stays deleted through prefab updates until `prefab.revert`. Prefabs nest: `prefab_create` keeps the instances inside the subtree live, and only the outermost instance records overrides. `prefab_apply` writes an edit inside a nested instance to the outermost prefab, or with `level` to the prefab of an instance on the way down (`{instance: "/Room/Lamp", level: "/Room/Lamp"}` writes Lamp.prefab), then rewrites the prefabs nesting it (`updatedPrefabs`) and rebuilds their instances; `prefab.unpack` unpacks one level, from the outermost.
- **Per-instance values.** `prefab_instantiate`'s `components` sets instance overrides in the same command, as the inspector would: `{<Component>: {<Field>: value}}`, script fields through `Script: {Scripts: [{Fields: {...}}]}`, one entry per root slot, replacing the root's list: an entry with `Fields` but no `Script` patches that slot (its script, `ExecutionOrder` and other fields kept; `null` removes a field). It works inside `edit_batch` and with `dryRun`, so a grid of variants is one op per cell:

```json
{"label": "Brick grid", "ops": [
	{"method": "prefab.instantiate", "params": {"prefab": "Assets/Prefabs/Brick.prefab", "parent": "/Bricks", "name": "Brick_1_01",
		"transform": {"Translation": [-7.2, 5.5, 0]},
		"components": {"MeshRenderer": {"Materials": ["Assets/Materials/BrickRed.material"]}, "Script": {"Scripts": [{"Fields": {"Points": 5}}]}}}}]}
```

## Tracks, levels and collisions

- **Tracks and levels.** Give each level a root entity with a `Static` `RigidBody` and instantiate every track piece under it: the pieces join one static compound body, so a rolling ball crosses seams without bumps. Separate static bodies that touch are reported as `PHYSICS_ADJACENT_STATIC_BODIES`, and `project_validate {fix: ["PHYSICS_ADJACENT_STATIC_BODIES"]}` adds that root body for you. Lay out a level as one `edit_batch` of `prefab.instantiate` ops under the root (`"parent": "/Level"`), plus ordered waypoint entities along the main route for the autopilot.
- **Entities that report their own collisions** (bricks, targets, pickups): a scripted collider merged into a root's compound body gets its own enter and exit, but at a seam only one piece may be notified (`luau-gameplay`, "Per-entity collisions"). So pieces that must each report a hit at a shared seam, such as Breakout bricks, keep their own `Static` `RigidBody` (in the prefab) under a root without a body, with a 0.1 m gap between neighbours so they never touch and `PHYSICS_ADJACENT_STATIC_BODIES` stays quiet; scripted pieces never hit on a shared seam (targets, pickups apart from each other) may share one static root. Walls the ball only bounces off can share one compound body under a root. `luau-gameplay` has the body settings of a constant-speed ball and a player-driven paddle.
