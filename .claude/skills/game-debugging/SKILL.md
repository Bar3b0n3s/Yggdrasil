---
name: game-debugging
description: Find out why a game built with this engine misbehaves, through the editor's observability methods (logs, events, script errors, evaluation, scene outlines and diffs, annotated screenshots, physics, bounds, stats, audio, play state), and map every RPC, engine, script, bridge and validation error code to its fix.
---

# Game debugging

Use this skill when a game built through automation does something unexpected: a script error, a test that fails, a ball that falls through the track, a sound that is silent, a replay that no longer verifies. `docs.get {topic: "reference/Automation"}` has every method's params and results, and `reference/Validation` every validation code (the topics `game-building` lists under "Documentation"; Architecture §13.7 of the repository describes the observability design). Methods are written `domain.verb` as `engine_client` calls them; the MCP bridge proxies the tool-flagged ones as `domain_verb` (`log.read` is the tool `log_read`) and reaches every other method through `engine_call {method, params}`. Work in a loop: **observe, form one hypothesis, check it with the narrowest query, fix through automation, re-run the deterministic check.** Never edit project files by hand to try something (see `game-building`).

## The observability loop

1. **Read what the engine already told you.** Every response carries `_meta.diagnostics` (`newErrors`, `newWarnings`, `newScriptErrors`, `firstNew`) and `_meta.playState`. Start there. `_meta.revision` counts changes to the open scene and the project settings only: creating or writing assets and scripts leaves it unchanged, so `ifRevision` guards the scene and the settings, not asset files.
2. **Logs and events, by cursor.** `log.read {cursor, minLevel: "Warn", contains?, limit?}` returns structured entries with source locations and entity context, oldest first, at most `limit` per call, and `nextCursor`. Page by passing exactly the returned `nextCursor` as the next `cursor` until a read returns no entries; never compute or guess a cursor (from `seq` numbers or entry counts), which skips entries without an error. `cursor: ""` starts at the oldest entry held, `cursor: "end"` returns only the cursor of the next entry (to watch from now on), and `dropped` counts entries overwritten before the read could see them. `events.read {cursor, types?: ["ScriptErrorRaised", "AssetImportFailed", "PlayStateChanged", "AssetReloaded", "SceneChangedOnDisk", "DiagnosticsChanged", "GpuDeviceLost"]}` pages the same way and tells what happened in order.
3. **Script errors.** `script.errors {since}` lists deduplicated errors with kind, script, line, column, callback, entity, tick, count and traceback; `nextCursor` continues, `evicted` says older ones were dropped. A failing instance is disabled; with `Scripting.PauseOnError` the editor also pauses ordinary play at the first error. A `script.eval` that raises is answered with `ScriptError` (-32006, the located error in error data `scriptError`) and also enters this list with callback `Evaluate`: read `script.errors` before evaluating, or tell those entries from the game's by their callback.
4. **Ask the running game.** `script.eval {context: "Play", entity: "/Game", code: "return self.Board:ToAscii()"}` evaluates in the play VM with `self` bound to the entity's script instance and returns the value plus captured prints. `context: "Edit"` is read-only. During a recording, evaluate only reads: a write through an engine API (a component, an entity, `SetActive`, a session `Random` draw) invalidates the recording, and a write to a Luau table (`self.Score = 0`, `self.Board:FillRows(...)`) is neither detected nor recorded, so once that write shows in a checkpoint, the final hash or an expectation the recording no longer reproduces itself and `input.record`'s `stop` refuses it (a recording with input events) or its first `input.replay` fails (an event-free one).
5. **Where things are.** `scene.tree {format: "Text", root?, depth?}` gives the compact outline (ids, components, positions); `scene.query {where: {name?, tag?, component?, path?}}` finds entities; `entity.get {entity, components: "all"}` shows values; `entity.bounds {entities}` gives world AABBs, the quickest check for gaps and overlaps.
6. **What changed.** `scene.diff {against: "Saved"}` lists RFC 6902 changes per entity since the last save (or `against: "Revision", revision`); `edit.history` lists the undo steps. There is one command history: `scene.open` and `scene.new` clear it (earlier undo steps are gone), but undo indices keep increasing for the editor's lifetime, so an index never names a later command; an `undoIndex` of 0 means the call recorded no command (a `project.validate` that fixed nothing, a dry run, a batch op).
7. **Look.** `viewport.screenshot {view: "Game" | "Scene", width, height, target?, annotate: {labels: "all", colliders: true, bounds: true, axes: true}}` draws names, short ids, colliders and bounds over the frame so you can tie pixels to entities. For the game view pass the project window's size (`width: 720, height: 900` for a 720 × 900 game): the default is 640 × 360 whatever the window, which misframes a portrait game and scales screen text by the wrong height; `target: "Play"` renders the play scene, where runtime entities such as a pool exist; `debugView` shows a data view instead of the lit image: `Albedo`, `Normals`, `Roughness`, `Metallic`, `Emissive`, `AO`, `ShadowCascades` or `Overdraw` (`Lit` is the default). Needs an editor with `renderer: "vulkan"`.
8. **Physics.** `physics.bodyInfo {entity}` reports the body (type, layer, sensor, colliders, velocities, sleeping, bounds, contacts since which tick, grounded and ground normal) of the play session. A missing body means a physics validation error kept it from being created.
9. **Timing and load.** `stats.get` reports frame and per-pass timings, dropped simulation time, entity, body and voice counts, the Luau heap and its limits, and the GPU device losses the editor recovered from (`deviceLosses`). Its `views` are the editor's displayed viewport panels, and only displayed panels render: a headless editor displays the Scene view only, so `game` is unavailable there, and `play.step {render}` renders the play scene through the Scene view. `play.state` reports mode, tick, `stateHash`, lockstep owner, whether the session was modified, recording, and `stopped`/`stopError` after a fatal stop.
10. **Audio.** `audio.stats` lists the device, group volumes and every voice (clip, entity, volume, pitch, spatial position, cursor). No voice means nothing played; a voice at volume 0 points at the mixer or attenuation.
11. **Reproduce deterministically.** `play.start {lockstep: true, seed}` and `play.step {ticks, input}` reproduce a bug tick by tick; equal seeds and inputs must give equal `stateHash` values. A test that fails only sometimes is a determinism bug (unordered iteration, wall time, unseeded randomness, `^` with a non-trivial exponent: see `luau-gameplay`).

### Common situations

- **A replay no longer verifies.** `input.replay {path, verify: true, strictHash: true}` reports each `Expect` outcome, `finalTick`, `stateHash`, `stopReason`, `checkpointsChecked` and `checkpointFailed`. The usual failure is "replay verification failed at checkpoint tick N: expected state hash X, observed Y" (`checkpointFailed: true`, playback stopped at N): checkpoint N is the first whose state hash differed and every earlier one matched, so the divergence lies in the 60 ticks before N; bisect only there (a final-hash mismatch with every checkpoint matched lies after the last checkpoint). In `test.run` the replay's `Checkpoint@<tick>` or `FinalStateHash` row fails with both hashes. Either failure carries a hint: the world differs from the recording, because a serialized component of the scene (the look's included: `PostProcess`, `Environment`, lights, `Camera`, `Text`), a script, an asset or the engine changed since it was recorded, or the play is not deterministic. An edit of the scene's look changes every checkpoint, so its replays fail at their first. The session then stays paused at the replay's last tick (or where playback stopped): read its state with `script.eval {context: "Play"}`, `scene.query` or `entity.get`, and end it with `play.stop`. A hash mismatch after an intended change (an engine fix, gameplay, the look) means re-recording with `test.run {record}` (`game-building`, "Re-recording replays"); otherwise compare `play.step` hashes tick by tick against a fresh recording within that window to find the first divergent tick.
- **A recording is refused: "does not depend on its input".** `input.record {action: "Stop"}` or `test.run {record}` fails with `Validation` when the recording, replayed without its input events, still passes its verification (every checkpoint, the final hash and every expectation, whatever script errors that run raised); `input.record` puts that run's outcome in error data `inputCheck`, and nothing is written. That happens only when the input never shows in the world at a checkpoint (one every 60 ticks), at the end or in an expectation: a recording shorter than 60 ticks that ends idle, input that changes only script state, or play after the last checkpoint that a restart undoes (a longer recording that ends with a restart is accepted, its checkpoints having seen the play, but its final hash then proves only the restored state). End the recording while the played state is still in the scene, record again, and confirm with `input.replay {path, verify: true, strictHash: true, withoutInput: true}` that the new replay fails without its input. An accepted `test.run {record}` reports its replay in the `recording` block, whose `inputCheck.divergedAtTick` is where the run without input first differed (`input.record`'s `inputCheckTick`).
- **A recording is refused: "does not reproduce itself".** Before its input check, `input.record {action: "Stop"}` replays a recording with input events with its input (`verify`, `strictHash`, its `expect`) and refuses with `Validation` at `/path` naming the first failure in tick order: a checkpoint (`the state hash at checkpoint tick <t> is <observed>, recorded <expected>`), an expectation (`expectation <i> at tick <t> (/expect/<i>/luau) is false`, or `failed: <message>`), the final hash, where playback stopped, or the playback's first script error. Error data `verification` holds the playback's outcome; nothing is written, the recording has ended and the session is the verification's, paused where it stopped. The usual causes: a `script.eval` or `play.waitFor` that wrote script state during the recording, an expectation that does not hold with the recording's own input (check it first with `script.eval {context: "Play"}` at its tick), and a script that reads the host or iterates entity keys with `pairs` (`luau-gameplay`). Record again without them.
- **`test.run` refuses: "is open with unsaved changes".** A selected suite is bound to the open scene, or a selected replay plays it, and runs play saved scene files: `scene.save`, then run again.
- **A `test.run {record}` is refused because a row did not pass.** The message lists the selected rows that did not pass, `<suite>/<case> (<status>)`. A `replay:<path>/...` row is a committed replay the filter also selected: give the recording suite a name that no replay path contains (`Record.<Name>`) and record with exactly that filter.
- **A soak fails a scene.** The milestone gate plays every scene of a game in `Projects/<Game>` for 10,000 lockstep ticks without input and fails it on growing entity, body, voice or allocation counts, a growing memory or script heap, or a new script error (`OnStart` included). Reproduce it: `play.start {lockstep: true}` without a seed, `play.step` to tick 10,000, `script.errors` (before any `script.eval`, item 3), and `stats.get` past ticks 5,000 and 9,500; a per-lock `Scene.Instantiate`, pooled entities with bodies, a looping or late game-over sound and unbounded histories are the usual causes.
- **A test case fails.** `test.run {filter: "<suite>/<case>"}` reruns one case; its row has the failure messages with file and line, and `Test.ExpectScriptError` claims expected errors. A failed `Test.ExpectEqual` or `Test.ExpectNear` shows its message followed by `(got <actual>, expected <expected>)` (and `, epsilon <e>`), so pass the actual value first. Add `print` calls or `Test.GetStateHash()` checkpoints, then remove them.
- **A result too large to answer inline.** A response over 48 KB is `{path, truncated: true, summary}`: read the file with the bridge's `result_read {path, pointer?, offset?, length?}` (one page with `nextOffset`, or the JSON value at a pointer such as `/cases/0`; `engine_client.load_offloaded` outside the bridge), never with another file tool; it also reads a test run's `jsonPath` and `junitPath` reports. A `test.run` result names its failing rows in `failedCases` and counts every status in `counts`, inline. A `docs.get` topic too long for one response comes back as its first page with `nextOffset` and its sections; ask for a `section` or page on with `offset`.
- **A ball falls through or bumps.** `entity.bounds` on the neighbouring pieces shows gaps; `project.validate` reports `PHYSICS_ADJACENT_STATIC_BODIES` and layer problems; `physics.bodyInfo` shows the layer and contacts; check `Physics.Collisions` pairs the layers.
- **A script or asset change does nothing.** Lockstep, recording, replay and test sessions defer script and asset reloads (materials, sound effects, textures) until the session ends, so an `asset.setProperties` shows only after `play.stop` and a new `play.start`, while `asset.getProperties` (the tool `asset_get_properties`) reads a material's or sound effect's values from its file at once; ordinary play reloads scripts at the next safe point and assets at the next frame. To try component values in a running session, `entity.update {target: "Play"}` changes the play scene alone. A reload refused for type errors (with `BlockPlayOnTypeErrors`) or compile errors keeps the old code and reports a `type` or `compile` error with callback `Reload`.
- **`project.upgrade` refuses.** A material, sound effect, replay or sprite sheet with a member its reader does not know fails the upgrade at its file and pointer instead of losing the member; an open scene with unsaved changes is refused (save it first), while no open scene is needed.
- **The editor died.** The bridge reports `EditorCrashed {exitCode, lastLogLines, crashReportPath, autosaveAvailable}`; read the crash report, then `editor_launch {recover: true}` if an autosave exists.

## Error codes and fixes

### JSON-RPC errors (§13.3)

Every error response carries `error.data.errorCode` (the engine `ErrorCode` below) and `issues[]` of `{pointer, message, hint?, suggestions?}`; fix the value at each pointer.

| Code | Name | Fix |
|---|---|---|
| -32700 / -32600 | Parse error / Invalid request | Send one well-formed JSON-RPC 2.0 object per message. |
| -32601 | Method not found | Use a method `rpc.discover` lists; the hint suggests close names. |
| -32602 | Invalid params | Correct each `issues[]` pointer (unknown field, wrong type, out of range, missing required param). |
| -32000 | Internal | An engine bug: read `log.read` and the crash report, then report it with the request. |
| -32001 | NotFound | The entity, asset, component, file or topic does not exist: check the path or id; follow the suggestions. |
| -32002 | InvalidState | Not allowed now (no project or scene open, a test run owns the editor, another client owns lockstep, not playing): reach the needed state first. |
| -32003 | ValidationFailed | The input breaks a rule: fix each issue, then retry. |
| -32004 | Conflict | `ifRevision` no longer matches: re-read the state and retry with the current revision. |
| -32005 | Timeout | The call exceeded its deadline: bound the work (fewer ticks per `play.step`) or retry. |
| -32006 | ScriptError | A script raised during the call: read the located error and fix the script. |
| -32007 | Busy | The editor is in a long operation: retry after it ends. |
| -32008 | Unauthorized | Mutations are denied (read-only project or the deny switch): open a writable project or allow automation. |
| -32009 | Unsupported | Not available here (`dryRun` on a method without it, screenshots with `renderer: "none"`). |
| -32010 | Cancelled | The request was cancelled (client disconnect, project closed): repeat it if still needed. |

### Engine `ErrorCode` (`error.data.errorCode`, script and log messages)

| ErrorCode | Meaning | Fix |
|---|---|---|
| `InvalidArgument` | A parameter is malformed or out of range. | Correct the parameter the location names. `input.replay`'s `strictHash` and `withoutInput` each require `verify: true`. An `Entity` value inside component data (a script's `Field.Entity()`) takes the entity's 16-hex id, never a path: `{"$ref": "<op>.entity.id"}` in a batch, or the id `entity.get` returns. A `$ref` path that names no member says which object it inspected (`'0.prefab' (in the result of op 0)`) and lists up to 32 of the members that object has. |
| `NotFound` | A file, entity, asset, component or method does not exist. | Fix the reference; check spelling and case (paths are case-sensitive everywhere). |
| `AlreadyExists` | The name, path or id is taken. | Choose another path or open the existing item. |
| `InvalidState` | Not allowed in the current state. | Reach the state the message names (open a project, stop play, wait for a test run). "no scene open" after a launch or attach: `scene_open {path}` (the project's `StartScene`); `editor_status` shows the open scene as `session.scene`. `test.run`'s "is open with unsaved changes": `scene.save` first. `asset.delete` of a folder holding a file whose name no project path can spell (such as `nul.txt` or one ending in a dot) removes nothing: remove that file outside the editor. `entity.destroy` of a prefab instance's member (not its root) in the edit scene: deactivate it (`entity.update {active: false}`) or `prefab.unpack` the instance first, as the hint says; `prefab.create {replaceWithInstance}` of one: `prefab.unpack` the instance first, or save the prefab without replacing. |
| `Io` | The operating system refused a file operation. | Check the path, disk space and that no other process locks the file. |
| `Parse` | Malformed JSON or truncated data. | Fix the syntax at the reported line and column. |
| `Validation` | Well-formed input breaks a rule. | Fix the value at the JSON pointer; the hint says how. A recording refused because it "does not depend on its input": end it while the played state is still in the scene, record again, and confirm with `input.replay {verify: true, strictHash: true, withoutInput: true}` ("Common situations"). |
| `UnsupportedVersion` | A file is newer than this build. | Use a newer engine, or restore the file from the version this engine wrote. |
| `ImportFailed` | An asset failed to import. | Fix the source file; `asset.info` shows its diagnostics. |
| `CompileFailed` | Luau source did not compile. | Fix the syntax at the reported location (`script.check`). |
| `Script` | A script raised, or a reload was refused for its errors. | Fix the script at the reported location. |
| `Gpu` | The GPU device failed or is missing, or was lost during the call. | Use `renderer: "none"` for logic-only work, or check the Vulkan driver. A lost device is recovered at the next frame boundary, play going on as if nothing happened: wait for the `GpuDeviceLost` event (`events.read {types: ["GpuDeviceLost"]}`), then repeat the request. |
| `Timeout` | An operation exceeded its budget. | Bound the work or raise the budget the message names. |
| `PermissionDenied` | A write to a read-only project or mount. | Open the project writable; never write under `engine://`. |
| `Unsupported` | Not available in this host or configuration. | Use a host that supports it (an editor with a renderer, a non-Dist build). |
| `Conflict` | An optimistic-concurrency mismatch. | Re-read and retry with the current revision. |
| `Cancelled` | The operation was cancelled. | Repeat it if it is still wanted. |

### Script error kinds (`script.errors` rows, §11.7)

| Kind | When | Fix |
|---|---|---|
| `compile` | The source or a required module does not compile, a require does not resolve, or the module top fails at load time (an engine call at the module top, an error, the 250 ms load budget). Also a reload refused for these. | Fix the reported location; keep module tops free of engine calls (do that work in callbacks). |
| `type` | A reload refused because the new source has type errors while `Scripting.BlockPlayOnTypeErrors` is set, or in a test run (callback `Reload`). The running code is kept. | Fix the type error at the reported location (`script.check`), then save again to reload. |
| `runtime` | A callback, task or test case raised. The instance is disabled. | Fix the line in the traceback; guard optional values (`if self.Goal then`). |
| `timeout` | A callback ran past `Scripting.CallbackBudgetMs`. | Remove the unbounded loop, or spread the work over ticks with a task. |
| `memory` | The VM passed `Scripting.MemoryLimitMB`; a second breach stops the session. | Stop allocating per tick (pool tables and entities), or raise the limit deliberately. |

### MCP bridge errors

| Error | Fix |
|---|---|
| `EditorCrashed {exitCode, lastLogLines, crashReportPath, autosaveAvailable}` | Read the crash report and the last log lines; relaunch with `editor_launch {recover: true}` when an autosave exists. |
| `EditorAlreadyOpen {pid}` | An editor without automation holds the project: enable "Allow AI automation" in its Automation panel, then `editor_attach`. |
| `LockUnavailable` | A gate (`CI.py`) runs, or, for `renderer: "vulkan"`, another GPU run holds the machine-wide GPU lock (the message names its pid, worktree and command; `python Scripts/Locks.py` lists the holders): call `editor_launch` again once it has ended, or launch with `renderer: "none"` when no screenshot or smoke test is needed and no gate runs. |
| `NotConnected` | Call `editor_launch` or `editor_attach` first. |
| `ConnectionClosed` | The editor closed the connection: check `editor_status`, then relaunch or reattach. |
| `Timeout` | The call exceeded the method's `timeoutSeconds`: retry with less work. |
| `UnknownTool` | Use a tool the bridge lists; reach any other method through `engine_call {method, params}`. |
| `InvalidParams` / `InvalidState` | Fix the bridge tool's arguments, or `editor_shutdown` before launching another editor. |
| A missing editor binary | Build it: `python Scripts/Build.py --config Release --project Editor`. |

### Validation codes (`project.validate`, `reference/Validation`)

`project.validate {fix: true}` applies every safe fix; `fix: ["<id or CODE>"]` only the selected ones, as one undoable command. Each diagnostic's `autoFixable` says whether that one can be fixed automatically.

| Code | Severity | What it means | Fix |
|---|---|---|---|
| `SCENE_NO_PRIMARY_CAMERA` | Warning | A scene with entities has no Primary camera. | Set Primary on the camera, or add one with `entity.create {components: {Camera: {Primary: true}}}`; the fix makes a scene's only camera Primary. |
| `SCENE_MULTIPLE_PRIMARY_CAMERAS` | Error | Several cameras are Primary. | Clear Primary on all but one; the fix keeps the first in scene order. |
| `SCENE_DUPLICATE_UNIQUE_COMPONENT` | Error | A scene file holds two Environment or PostProcess components. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_NONCANONICAL_ORDER` | Warning | A child is listed before its parent in a scene file. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_INVALID_HIERARCHY` | Error | A Parent names no entity, or parents form a cycle. | `scene.open {repair: true}`, then `scene.save`. |
| `SCENE_INCONSISTENT_PREFAB_LINK` | Error | A prefab member's PrefabLink does not match its instance root. | `scene.open {repair: true}` (unpacks the member), then `scene.save`, or re-instantiate the prefab. |
| `ENTITY_DUPLICATE_ID` | Error | Two entities of a file share an ID. | `scene.open {repair: true}`, then `scene.save`. |
| `ENTITY_DANGLING_REFERENCE` | Warning | An entity reference names no entity of its scene. | Point it at an existing entity; the fix clears it in the open scene. |
| `COMPONENT_FIELD_OUT_OF_RANGE` | Error | A component value in a file fails the registry's checks. | Correct the value through automation, or `scene.open {repair: true}` (drops the component). |
| `COMPONENT_MISSING_REQUIREMENT` | Error | A component lacks a component it requires. | Add the required component, or repair-load the scene. |
| `COMPONENT_CONFLICT` | Error | A component shares its entity with one it excludes. | Remove one of them, or repair-load the scene. |
| `ASSET_MISSING` | Error | A reference names a handle no asset has. | Restore the asset (`edit.undo` of its deletion) or assign another. |
| `ASSET_TYPE_MISMATCH` | Error | A reference names an asset of another type, or a .meta disagrees with its source. | Assign an asset of the accepted type; the fix rewrites a mismatching .meta for the importer that takes the source, keeping its handle. |
| `ASSET_IMPORT_FAILED` | Error | An asset, its .meta or a scene file cannot be imported or loaded. | Fix the source file; `asset.info` shows the failure. |
| `ASSET_ORPHAN_META` | Warning | A .meta has no source file. | Restore the source, or let the fix trash the .meta. |
| `ASSET_DUPLICATE_HANDLE` | Error | Copied .meta files share a handle. | Let the fix give each copy a fresh handle. |
| `ASSET_ORPHAN_DEPENDENCY` | Warning | A dependency .meta names a missing owner. | Restore the owner, or let the fix trash the dependency .meta. |
| `ASSET_TANGENTS_APPROXIMATED` | Warning | A glTF primitive needs tangents but has no UVs. | Export the model with UVs or tangents. |
| `ASSET_UNSUPPORTED_UV_SET` | Warning | A glTF texture uses TEXCOORD_1, which is ignored. | Bind it to TEXCOORD_0 in the model. |
| `ASSET_VERTEX_COLORS_IGNORED` | Warning | A glTF primitive has vertex colours, which are ignored. | Bake them into a texture or material. |
| `ASSET_CONTENT_SKIPPED` | Warning | A glTF holds skins, animations, cameras, lights, points or lines. | Remove them, or rebuild cameras and lights as components. |
| `PATH_CASE_MISMATCH` | Error | A .meta or path matches a file only ignoring case. | Use the exact spelling; the fix renames the .meta. |
| `PREFAB_MISSING_ASSET` | Warning | An instance names an unregistered prefab. | Restore the prefab, or `prefab.unpack` the instance. |
| `PHYSICS_NONCONVEX_DYNAMIC` | Error | A non-convex MeshCollider is on a Dynamic body. | Set Convex, or make the body Static or Kinematic. |
| `PHYSICS_MIXED_TRIGGER` | Error | A body's colliders disagree on IsTrigger. | Make all or none triggers; move triggers to a child with its own body. |
| `PHYSICS_DYNAMIC_TRIGGER` | Error | A Dynamic body has only trigger colliders. | Make it Kinematic or Static, or clear IsTrigger. |
| `PHYSICS_ALL_DOFS_LOCKED` | Error | A Dynamic body has every axis locked. | Unlock an axis, or make it Kinematic or Static. |
| `PHYSICS_INVALID_SHAPE` | Error | A collider shape cannot be created. | Fix its size or scale, or use a mesh with volume. |
| `PHYSICS_ADJACENT_STATIC_BODIES` | Warning | Touching static bodies make a rolling body bump. | The fix adds a Static RigidBody to their common ancestor (one compound). |
| `PHYSICS_UNKNOWN_LAYER` | Error | A body names an undeclared layer (it uses Default). | Declare it in `Physics.Layers`, or name a declared layer. |
| `PHYSICS_NONUNIFORM_SCALE` | Warning | A sphere or capsule under non-uniform scale. | Use a uniform scale or a box/convex collider. |
| `PHYSICS_DYNAMIC_UNDER_MOVING_PARENT` | Warning | A Dynamic body below a moving ancestor does not follow it. | Move it out from under the ancestor, or make it Kinematic. |
| `PHYSICS_LIMIT_EXCEEDED` | Warning; Error at the limit | Too many bodies for the world's limit. | Merge colliders into compound bodies or remove bodies. |
| `SCRIPT_COMPILE_ERROR` | Error | A script does not compile or fails at load time. | Fix the reported line (`script.check`). |
| `SCRIPT_TYPE_ERROR` | Error; Warning for a Luau lint warning | The strict checker reports a finding other than a syntax error (a type error, an unsupported type function, a require it cannot resolve); one of Luau's lint warnings (an unused local, two statements on one line) is a Warning. | Fix the reported range (`script.check`); the games suite fails a game on either. |
| `SCRIPT_NOT_A_BEHAVIOUR` | Error | A Script component names a Module or TestSuite. | Assign a script that returns `Script.Define`. |
| `SCRIPT_UNKNOWN_FIELD_OVERRIDE` | Error | An override names no declared field. | Remove or rename the override. |
| `SCRIPT_FIELD_TYPE_MISMATCH` | Error | An override does not match its declared type. | Correct the override's value. |
| `INPUT_UNKNOWN_ACTION` | Error | An `Input` call names an undeclared action. | Declare it in `Input.Actions`, or fix the literal. |
| `AUDIO_NO_LISTENER` | Warning | Spatial sources but no primary listener or camera. | Add a Primary AudioListener to the camera or player. |
| `AUDIO_MULTIPLE_PRIMARY_LISTENERS` | Warning | Several listeners are Primary. | Clear all but one; the fix clears the later ones. |
| `RENDER_NO_LIGHTING` | Warning | Non-emissive meshes but no light or ambient light. | Add a light, or raise the environment's intensity. |
| `RENDER_LIGHT_LIMIT_EXCEEDED` | Warning | More than 256 enabled lights. | Disable or remove lights. |
| `RENDER_SPOT_SHADOW_BUDGET` | Warning | More than 8 shadow-casting spot lights. | Turn off shadows on less important spot lights. |
| `BUILD_START_SCENE_MISSING` | Error | StartScene names no scene file. | Create it with `scene.new`, or point StartScene at a scene. |
| `BUILD_SCENE_MISSING` | Error | An Export.BuildScenes entry names no scene file. | Create the scene; the fix removes the entry. |
| `TEST_SUITE_INVALID` | Error | A Testing.Suites entry's script is not a TestSuite, or its scene is not a registered scene asset. | Assign a `Test.Suite` script; create and save the scene (so it has its `.meta`), or leave Scene empty. |
