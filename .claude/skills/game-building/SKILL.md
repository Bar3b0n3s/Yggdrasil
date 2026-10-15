---
name: game-building
description: Build a game end to end through the editor's automation (the MCP tools or engine_client): launch, project settings, scenes, prefabs, scripts, deterministic checks, test suites, recorded replays with the autopilot workflow, validation and export, and the provenance, transcript and demo-audit rules that define done.
---

# Game building

This is the workflow an agent follows to build a game with this engine, from an empty project to an exported build. Write gameplay with the `luau-gameplay` skill, and diagnose with `game-debugging`. Tools are named `domain_verb` as the MCP bridge proxies them; a method without a tool (`prefab.revert`, `project.refreshAssets`, `play.state`) is reached through `engine_call {method, params}`, and `engine_client` calls every method by its `domain.verb` name.

## Documentation

Everything a builder reads is served by `docs_get`, which needs a connected editor: launch or attach first ("1. Launch"). `docs_get {}` lists every topic with its size.

- **Skills:** `skills/<name>`; this file is `skills/game-building`.
- **Reference:** `reference/Automation` (every method with its params and results), `reference/Components` (every component field with its range and default), `reference/FileFormats`, `reference/Validation` and `reference/ScriptAPI`.
- **JSON schemas** of the file formats: `reference/Schemas/<Format>`, such as `reference/Schemas/Project` (every `.eproj` field) or `reference/Schemas/SoundEffect`.
- **Scripting:** `scripting/Engine.d.luau` (the declarations the type checker uses) and the script templates `templates/Scripts/Behaviour`, `templates/Scripts/Module` and `templates/Scripts/Test`.
- **Long topics:** a topic too large for one response comes back as its first page with `nextOffset` and its list of sections. `{topic, toc: true}` returns only the list, each section's `title`, `anchor`, `level`, `offset` and `bytes` (add `section` for its subsections). Ask for one section by its heading's exact text or its anchor (`{topic, section: "Input"}`, `"reference/ScriptAPI#input"`); a heading that would share its name with another carries its kind, such as `"Script (component)"` beside the `Script` global or `"TestSuite (type)"` beside `Test.Suite`, and the plain name (`"TestSuite"`) finds it too. Page on with `{topic, offset: <nextOffset>, length?}`.
- **One method's JSON schemas:** `engine_call {method: "rpc.discover", params: {method: "prefab.instantiate"}}`.

Architecture sections cited below (§13.12 is the demo acceptance rule) and the `Scripts/` commands of the definition of done belong to the repository, for the people who review and gate the game; an automation-only builder needs only the topics above.

## Ground rules

- **Only through automation.** Every engine-consumed file of a game (the root `<Game>.eproj` and `.luaurc`, everything under `Assets/` and every `.meta`) is written by the editor through an automation method: never with a text editor, a shell or a file-copy tool. `Scripts/AuditProvenance.py` rejects any other origin (§13.12).
- **Provenance and transcript are committed.** In the game's project directory, `<Game>/` below (`Projects/<Game>/` for a demo, `Tests/Projects/Breakout/` for the M16 dry run), the editor records each write in `<Game>/Automation/Provenance.json` (`{Path, XXH64, Method, RequestId, Client, TranscriptLine}`; `RequestId` is null only for a `ui` write), and the MCP bridge appends every request to `<Game>/Automation/BuildLog.jsonl` and passes its line number on, so each entry names the transcript line of the request that wrote it. Never edit either by hand; never delete the transcript.
- **Windowed UI writes do not count.** A file saved from the editor's own UI is recorded with method `ui`, which the audit rejects for demo games. Use the UI to look, not to author.
- **`Library/` is a cache** (import cache, autosave, lock, offloaded results). It is gitignored and never consulted by the audit.
- **Documentation is exempt.** `<Game>/AGENTS.md` and a `README.md` may be written directly; describe the game's scenes, actions, scripts and tests there when you finish.
- **A missing capability is an engine change.** When automation cannot do what the game needs, add the method (the `add-automation-method` skill) or the script API (`add-script-api`) with its tests before continuing. Never work around it by hand.

## 1. Launch

- `editor_launch {project: "Projects/<Game>", create: true, template: "Empty"}` creates and opens the project (templates: `Empty`, `Basic3D`). A relative project path (`"Projects/<Game>"`, `"Tests/Projects/Breakout"`) names a directory below the repository root, whatever the bridge's working directory, and the transcript records it that way. Without `create` it opens an existing one, always through a transcribed `project.open` on a launcher-state editor (never `--project`), so whatever opening writes, such as the `.meta` files of new sources, is attributed to that request. The bridge launches the Release editor headless with `renderer: "none"`; for a full build pass `renderer: "vulkan"`, which screenshots and a Dist export's smoke test need (a `--renderer none` editor answers screenshots with `Unsupported` and leaves the Dist smoke test unrun).
- If the user already has the project open with "Allow AI automation" on, `editor_launch` attaches to that editor instead (§13.8). `EditorAlreadyOpen {pid}` means an editor without automation holds the project: ask the user to allow automation, then `editor_attach`. Every attach and every launch begins with a transcribed `session.info` (a launch sends it right after its `project.open` or `project.create`), so the transcript shows where each session starts and the result reports the session with its open scene. Request ids start again with every launch or attach and can skip where the bridge read `rpc.discover` for itself (`engine_call`'s timeout; those reads are not transcribed), so the transcript line, not the id, identifies a request.
- **No scene is open after a launch.** Opening a project opens none of its scenes, so scene and entity tools answer `InvalidState` ("no scene open") until you open one: `scene_open {path: "Assets/Scenes/Main.scene"}` (the project's `StartScene`, from `project_get_settings`), or `scene_new {path}` in a new project. An attached editor shows whatever scene its user has open, or none. `editor_status` (its `session.scene`) and the result of a launch or an attach report the open scene's `path` and `dirty` flag, or null.
- After `EditorCrashed {autosaveAvailable: true}`, `editor_launch {recover: true}` adopts the autosave as an unsaved scene; save it deliberately.
- `docs_get` answers only through a connected editor, so launch or attach before reading any topic ("Documentation" above).

## 2. Project settings

`project_set_settings {patch}` applies an RFC 7386 merge patch to the `.eproj` as one undoable command. Objects merge key by key; **arrays replace whole** (restate the full `Layers`, `Collisions`, `BuildScenes`, `Suites` or `Replays` list each time).

```json
{"patch": {
	"Name": "Tetris", "StartScene": "Assets/Scenes/Main.scene",
	"Window": {"Title": "Tetris", "Width": 720, "Height": 900},
	"Simulation": {"FixedHz": 60, "Seed": 42},
	"Input": {"Actions": {
		"MoveLeft": {"Type": "Button", "Bindings": ["Key.Left", "Gamepad.DPadLeft"]},
		"MoveX": {"Type": "Axis", "Positive": ["Key.D"], "Negative": ["Key.A"], "Gamepad": "Gamepad.LeftX"}}},
	"Export": {"BuildScenes": ["Assets/Scenes/Main.scene"]},
	"Testing": {
		"Suites": [{"Script": "Assets/Tests/Board.test.luau"},
			{"Script": "Assets/Tests/Tetris.test.luau", "Scene": "Assets/Scenes/Main.scene"}],
		"Replays": ["Assets/Tests/Replays/*.replay"]}}}
```

- `project_get_settings` returns the whole document; `docs_get {topic: "reference/Schemas/Project"}` lists every field with its type, range and default.
- Physics layers: `Physics.Layers` holds 1 to 16 names with `Default` first, and `Physics.Collisions` lists the pairs that collide; unlisted pairs do not.
- `_meta.diagnostics` in every response reports new validation errors. `BUILD_START_SCENE_MISSING` is expected until the start scene exists.

### `Testing.Suites`

Each entry is `{Script, Scene?, Parameters?, Isolation?, Clock?, Overrides?, ExpectQuit?, Modes?}` (§11.10):
- `Script`: the `*.test.luau` file; `Scene`: the scene it runs in (omitted: an empty scene, right for pure-logic suites).
- `Parameters`: the `Scene.Load` parameters (`Scene.GetLoadParameters()`), for example a timer carried into level 2.
- `Isolation`: `"Suite"` (cases share one scene, in order) or `"Case"` (the scene restarts before each case).
- `Clock`: a list of frame deltas that replaces the lockstep clock (for frame-timing tests).
- `Overrides`: `{CallbackBudgetMs, MemoryLimitMB, PauseOnError}` for this suite, applied exactly.
- `ExpectQuit`: the exit code the suite must end with through `Application.Quit` (-1: it must not quit).
- `Modes`: the run modes it runs in, `["Editor", "Release", "Dist"]` by default; autopilot suites are `["Editor"]`.

`Testing.Replays` holds globs of `.replay` files; every run verifies each one. `CaseTimeoutTicks` and `SuiteTickLimit` bound cases and suites. Leave `CoverageGates` false: only `Projects/FeatureTest` evaluates the engine's coverage gates.

## 3. Scenes, assets and prefabs

- `scene_new {path: "Assets/Scenes/Main.scene"}` creates and opens a scene; `scene_save` writes it; `scene_open {path, save | discardChanges}` switches.
- Build content with one atomic `edit_batch {label, ops: [{method, params}]}` per logical step: if op *k* fails, ops 0..k−1 are undone and the error names `failedOp`. `{"$ref": "<op>.<jsonPath>"}` passes an earlier result on, such as `{"$ref": "3.entity.id"}` for a parent or `{"$ref": "0.asset.id"}` for a material. `dryRun: true` checks a batch without applying it.
- `entity_create {name, parent?, components: {Type: {Field: value}}}` and `entity_update` take component values by registry name; `component_schema {name}` lists fields, ranges, defaults and enum values. Entities are referenced by id, unique id prefix or path (`/Game/Board`); assets by handle, `Assets/...` path or `engine://...` path (built-ins such as `engine://Meshes/Cube`).
- `asset_create {type: Material | SoundEffect | Scene | Prefab | Folder, path, values}` writes native assets; `asset_import {source, destDir}` imports external files (glTF, PNG, HDR, audio, fonts). `asset_delete {asset}` moves an asset with its `.meta` to `Library/Trash/` as one undoable command; given a folder below `Assets/` that holds no files (only empty folders, as a trashed recording leaves behind), it removes that folder, also undoably, and it refuses a folder that holds files (`Validation`, naming them: delete its assets first) or a file whose name no project path can spell, such as `nul.txt` or one ending in a dot (`InvalidState`, nothing removed: remove it outside the editor).
- Prefabs: build an entity, `prefab_create {entity, path, replaceWithInstance?}`, then `prefab_instantiate {prefab, parent?, index?, transform?, name?, components?}`; `prefab_apply` writes an instance's overrides into the prefab and `prefab.revert` (through `engine_call`) drops them. `prefab_create` returns the replacing instance as `instance`, `prefab_instantiate` returns the new root as `entity` (`{"$ref": "2.entity.id"}` in a batch).
- **Per-instance values.** `prefab_instantiate`'s `components` sets instance overrides in the same command, as the inspector would: `{<Component>: {<Field>: value}}`, script fields through `Script: {Fields: {...}}`. It works inside `edit_batch` and with `dryRun`, so a grid of variants is one op per cell, with no `entity_update` after each:

```json
{"label": "Brick grid", "ops": [
	{"method": "prefab.instantiate", "params": {"prefab": "Assets/Prefabs/Brick.prefab", "parent": "/Bricks", "name": "Brick_1_01",
		"transform": {"Translation": [-7.2, 5.5, 0]},
		"components": {"MeshRenderer": {"Materials": ["Assets/Materials/BrickRed.material"]}, "Script": {"Fields": {"Points": 5}}}}},
	{"method": "prefab.instantiate", "params": {"prefab": "Assets/Prefabs/Brick.prefab", "parent": "/Bricks", "name": "Brick_1_02",
		"transform": {"Translation": [-5.6, 5.5, 0]},
		"components": {"MeshRenderer": {"Materials": ["Assets/Materials/BrickRed.material"]}, "Script": {"Fields": {"Points": 5}}}}}]}
```

- **Tracks and levels.** Give each level a root entity with a `Static` `RigidBody` and instantiate every track piece under it: the pieces join one static compound body, so a rolling ball crosses seams without bumps. Separate static bodies that touch are reported as `PHYSICS_ADJACENT_STATIC_BODIES`, and `project_validate {fix: ["PHYSICS_ADJACENT_STATIC_BODIES"]}` adds that root body for you. Lay out a level as one `edit_batch` of `prefab.instantiate` ops chained with `$ref`, plus ordered waypoint entities along the main route for the autopilot.
- **Entities that report their own collisions** (bricks, targets, pickups) keep their own bodies instead: a collision callback names the entity that owns the body, so colliders merged into a root's compound body all report the root. Give each brick its own `Static` `RigidBody` (in its prefab), put the bricks under a root without a body, and leave a small gap (0.1 m) between neighbours so they never touch and `PHYSICS_ADJACENT_STATIC_BODIES` stays quiet. Walls the ball only bounces off can share one compound body under a root. The body settings of a constant-speed ball and of a player-driven paddle are idioms of `luau-gameplay`.
- Look at the result: `scene_tree {format: "text"}` for the outline, `entity_bounds` for layout and gaps, and `viewport_screenshot {view: "game", annotate: {labels: "all"}}` (needs `renderer: "vulkan"`).

## 4. Scripts

- `script_create {path, template: "Behaviour" | "Module" | "Test"}`, then `script_write {path, source}`. Each result carries compile and type diagnostics with full ranges; iterate until `script_check {}` reports none. Set `Scripting.BlockPlayOnTypeErrors` to keep type errors out of play: play, reloads and test runs then refuse them (test runs always do).
- The templates (`templates/Scripts/<Template>` in `docs_get`) name their type `Script_<File>`, a unique valid identifier: rename it (the local table, the exported alias and the `Script.Define` name) in the first `script_write`.
- `script_write` replaces the whole file. Keep scripts small modules (rules in a Module, the behaviour that drives them in another) so each write stays short.
- Attach a behaviour with `entity_update {entity, components: {Script: {Script: "Assets/Scripts/Game.luau", Fields: {...}}}}`. `_meta` and `project_validate` report overrides that match no declared field (`SCRIPT_UNKNOWN_FIELD_OVERRIDE`) or have the wrong type (`SCRIPT_FIELD_TYPE_MISMATCH`).
- Keep pure logic (boards, scoring, kick tables) in Modules without engine calls, so the empty-scene suites can test it.

## 5. Verify deterministically

- `play_start {lockstep: true, seed: 42}` decouples simulation from the wall clock; only `play_step {ticks, input?, render?}` advances it. Each result carries `tick` and `stateHash`.
- Input events are tick offsets from the step's start: `{tick: 0, type: "action", name: "MoveLeft", state: "tap"}` (down this tick, up the next). Analog actions take `value`, which holds on every later tick until another value replaces it: send `value: 0` to let go. A `down` button likewise stays down until an `up`.
- `script_eval {context: "play", entity: "/Game", code: "return self.Board:ToAscii()"}` reads state; `play_wait_for {until: "<luau returning a boolean>", timeoutTicks}` steps until a predicate holds.
- Repeated runs with the same seed and input must give the same `stateHash`. A difference is a determinism bug in the game: see the pitfalls in `luau-gameplay`.
- **What the state hash covers.** The simulated world only: every entity with its id, name, parent, active flag, tags and serialized components (a `Script` component's authored `Fields`, never its Luau instance state), the tick, the session's `Random` state, the id draws and every body's position, rotation, velocities and sleep flag. It never covers script tables (`self.State`, module locals), audio voices, input state or anything rendered. Two runs with equal hashes have equal worlds at that tick; a score or a counter that lives only in a script table is invisible to it until a Text, a transform or another component shows it. Observe script state with `Expect` (replays) and `Test.Expect*` (suites).
- `play_stop` returns to the edit scene unchanged.

## 6. Test suites

- One suite per area: pure-logic suites in the empty scene (`Board.test.luau`) and integration suites bound to a scene (`Tetris.test.luau`) that drive play with `Test.InjectAction`, `Test.WaitTicks`, `Test.WaitUntil` and `Test.GetStateHash`.
- `test_run {}` runs every suite and verifies every replay; `test_run {filter: "Tetris/hard drop"}` selects by a case-insensitive substring of `<suite>/<case>`, so `"Breakout/"` also selects `Autopilot.Breakout/...`: name the longer suite or a case when one suite's name ends another's (a `record` filter must select exactly one suite). The result has `passed`, `counts` (rows by status: `total`, `passed`, `failed`, `error`, `timeout`, `skipped`, `quit`), `failedCases` (up to 20 `"<suite>/<case>"` names), each case with its outcome and failures with file and line, and the `jsonPath` and `junitPath` reports. The `coverage` object is always there with its `registryFingerprint` and `gates`, but its `counters` list stays empty unless you pass `coverage: true` (the JSON report always holds them). A green run has no failed, errored, timed-out or skipped rows.
- Write a feature's tests first: stub the new function, write its cases and run them red, then implement it and run them green. `edit_undo` and `edit_redo` also take a `script_write` back and forth, so a test can be shown failing without the feature through automation.
- The same suites run headless with `Editor --headless --project Projects/<Game> --run-tests [--filter F]` and, in exported testing builds, with `Runtime --feature-test`.

## 7. Replays

A replay records the input applied per tick from tick 0, the state hash of every 60th tick (its **checkpoints**) and the final state hash (§6.7, §13.6). The checkpoints make a replay prove the trajectory of its run, not only where it ends.

- Save the scene first: a recording plays the saved scene asset its header names. In Edit mode, `input_record {action: "start", scene?, parameters?, seed?}` starts a lockstep session at tick 0 and records. Drive it with `play_step` input, then `input_record {action: "stop", path: "Assets/Tests/Replays/Opening.replay", expect: [{tick: 600, luau: "return Scene.FindByName('Game') ~= nil"}]}`.
- A hot reload or a play-scene edit during the recording invalidates it: `stop` then fails, writes nothing and names the reason.
- **The input check.** A recording with input events is replayed without them before it is written. When that run reproduces every checkpoint, the final hash and every `Expect` (whatever script errors it raises), the recording's outcome does not depend on its input, so it proves nothing about the gameplay you recorded: `stop` (and `test_run {record}`) refuse it with `Validation` ("does not depend on its input") and write nothing. That happens only when the input never shows in the world at a checkpoint, at the end or in an `Expect`: a recording shorter than one checkpoint interval (60 ticks) that ends idle, input that changes only script state (script tables are outside the state hash), or input that acts only after the last checkpoint and is undone by the end, such as by a restart. A longer recording that ends with a restart is accepted, because its checkpoints saw the play, but its final hash then proves only the restored state and the play after its last checkpoint goes unchecked. So end every recording while the played state is still in the scene (bricks broken, score shown, a life lost), and with `input_record` add an `Expect` for what only the input causes. A recording without input events (a pure determinism run) has nothing to check. The check replaces the play session, as `input_replay` does: after `stop` the session waits paused where the check ended (`inputCheckTick`); inspect the recorded run before `stop`, or replay the written file.
- Verify with `input_replay {path, verify: true, strictHash: true}`: every `Expect` must hold, every checkpoint must match when playback reaches its tick (the first that differs stops it there, `checkpointFailed`, naming the tick and both hashes) and the final hash must match. `checkpointsChecked` counts the checkpoints compared, the one that failed included, so a passing replay compared all of them. `Testing.Replays` makes every test run verify them; each replay's entry in the run's `suites` reports the same count as `checkpointsChecked` and the tick of a checkpoint that differed as `failedCheckpointTick`.
- `input_replay {path, verify: true, strictHash: true, withoutInput: true}` plays the replay with none of its input events. It must fail for a replay with events. Like every failed verification it fails as an error, -32003 `Validation`, whose data `replay` holds the outcome: `passed: false` and, at a differing checkpoint, `checkpointFailed: true`, `finalTick` at that checkpoint and `checkpointsChecked` counting it. When it passes, re-record the replay (the games suite checks this for every committed replay).
- After `input_replay` the session stays paused at the replay's last tick (or where playback stopped): inspect the end state with `script_eval {context: "play", ...}`, `scene_query` or `entity_get`, then end it with `play_stop`.

## 8. The autopilot-and-record workflow

Steering a physics ball with hand-written tick-stamped input is impractical. Instead write a test-only **autopilot suite** that steers like a player, record its run, and verify the replay without it:

1. Add `Assets/Tests/Autopilot/Level1.test.luau` to `Testing.Suites` bound to the level, with `Modes: ["Editor"]`, so exported builds never run it.
2. Its single case reads the ball's state, follows the level's waypoint entities with a PD controller, and injects analog actions with `Test.InjectAction` every tick until the goal is reached. It must only read the scene and inject input: never move entities, write components or draw from the session's `Random` stream, so the run equals a player pressing the same inputs.
3. Record it: `test_run {filter: "Autopilot.Level1", record: "Assets/Tests/Replays/Level1.replay"}`. The filter must select exactly one suite. The editor verifies the candidate in fresh ordinary gameplay without the test driver, every checkpoint and the final hash included, then runs the input check (§7), before it writes the file. Let the case end while its effects are in the scene, with no restart at its end (cover restarts in another suite): a restart after the last checkpoint leaves the final hash proving only the restored state. The result's `recording` block describes the replay written: `path`, `formatVersion`, `ticks`, `events`, `checkpoints`, `finalStateHash` and `inputCheck` (`ran`, and `divergedAtTick`, where the run without input first differed).
4. The replay is then verified **without** the autopilot by `input_replay {verify: true, strictHash: true}` and by every test run through `Testing.Replays`, which proves that plain input reaches the goal. Level 2 and 3 suites set `Parameters` (such as the carried timer), which the replay header stores. An autopilot recording carries no `Expect`: its checkpoints and the input check make it prove its gameplay, and behaviour assertions belong in suites (or in `input_record`'s `expect`).

```luau path=Assets/Tests/Autopilot/Level1.test.luau
--!strict
local BallScript = require("../../Scripts/Ball")

-- Steering gains of the PD controller and the distance at which a waypoint counts as reached.
local Gain, Damping, Reach = 0.6, 0.25, 1.5

local function byName(a: Entity, b: Entity): boolean
	return a.Name < b.Name
end

return Test.Suite("Autopilot.Level1", function()
	Test.Case("reaches the goal", function()
		local found = Scene.FindByName("Ball")
		Test.Expect(found ~= nil, "the level has a Ball")
		local ballEntity = found :: Entity
		local ball = ballEntity:GetScript() :: BallScript.Ball
		local body = ballEntity.RigidBody :: RigidBody
		local waypoints = Scene.FindAllByTag("PathNode")
		table.sort(waypoints, byName)
		local target = 1
		while not ball.ReachedGoal do
			local offset = waypoints[target].Transform.WorldPosition - ballEntity.Transform.WorldPosition
			if vector.magnitude(vector.create(offset.x, 0, offset.z)) < Reach and target < #waypoints then
				target += 1
			end
			local velocity = body:GetLinearVelocity()
			Test.InjectAction("MoveX", nil, math.clamp(offset.x * Gain - velocity.x * Damping, -1, 1))
			Test.InjectAction("MoveZ", nil, math.clamp(offset.z * Gain - velocity.z * Damping, -1, 1))
			Test.WaitTicks(1)
		end
	end, { TimeoutTicks = 3600 })
end)
```

## 9. Re-recording replays

When an engine change legitimately alters simulation results (a physics fix, a new serialized field, a changed default), replay hashes change and their verification fails:
- Re-run each autopilot suite with `test_run {record: ...}` through automation, so provenance and the transcript record the new replay.
- Replays without an autopilot (a Tetris opening) are re-recorded by repeating the recorded `play_step` inputs from the transcript inside a new `input_record` session.
- After a gameplay change, re-record a replay when it must cover the new behaviour: a matching hash only means the worlds matched at each checkpoint and at the end, and script state that never reached the world is not compared.
- The review of the change states why the hashes changed. A verifier's hash never replaces a recorded one by hand.
- A replay of format version 1 (no `Checkpoints`) still verifies, by its final hash only; `project_upgrade` rewrites it as version 2 without checkpoints, and re-recording it adds them.
- An engine change that alters the canonical form of project files also runs `project.upgrade` on every affected game (`project_upgrade {dryRun: true}` first, then `project_upgrade {}` through the bridge; `changes` gives each rewritten file with its reason, such as `"format version 1 to 2"` or `"canonical form"`; a material, sound effect or replay with a member its reader does not know fails the upgrade at its file and pointer instead of losing the member, and an open scene with unsaved changes is refused, while no open scene is needed; or `Editor --headless --project <Game> --upgrade`, which appends its own `cli` transcript lines: `project.refreshAssets` for its deferred asset scan, then `project.upgrade`), so the audit and the byte-identical re-save test stay green; the same change regenerates FeatureTest and re-records the affected replays.

## 10. Validate and export

- `project_validate {}` must report nothing. `fix: true` applies every safe fix as one undoable command; `fix: ["<id or CODE>"]` only the selected ones. `reference/Validation` explains each code and its fix.
- `project_export {config: "Dist", smokeTest: true}` writes `Build/<Platform>-Dist/<Game>/` and runs the exported executable with `--headless --frames 300 --expect-no-errors`; it must exit 0. Check the result: `smokeTestRan: true` and `smokeTestExitCode: 0`. A Dist Runtime needs a graphics device, so from a `renderer: "none"` editor the export succeeds with `smokeTestRan: false`, `smokeTestExitCode: null`, no `SmokeTest` entry in `phaseTimings` and a warning, which proves nothing: relaunch with `renderer: "vulkan"`. The Dist smoke test keeps the game's log in the user's real data folder (`%LOCALAPPDATA%\<Game>` on Windows), as a shipped game does. Export refuses script type errors, asset errors (also of content `Export.Exclude` leaves out, `Assets/Tests/**` included), missing build scenes, an `Export.Version` that is not `major.minor.patch` (each part at most 65535), an `Export.Icon` that is not a square PNG of at least 256 × 256, and an existing output directory that holds anything but an earlier export. On Windows the executable carries the icon, version and company (`resourcesPatched`); every package holds `THIRD_PARTY_NOTICES.txt`, and a macOS export is `<Game>.app`. The result's `phaseTimings` show where the time went.
- `project_export {config: "Dist", testing: true}` is a testing export (`Build/<Platform>-Dist-Testing/<Game>/`): it packages `Assets/Tests/**`, every `Testing.Suites` script and scene and every `Testing.Replays` match whatever `Export.Exclude` says, refuses invalid suites and replays whose expectations do not compile (issues at `/Testing/Suites/<i>` and `/Testing/Replays/<i>`), and its `Game.json` says `"Testing": true`, so `<Game> --headless --feature-test` runs the suites and replays in the shipping binary and writes `bin/TestResults/FeatureTest-<Mode>.{json,xml}` below its working directory, its user data in `<package>/bin/TestUserData/<Game>`. The result counts `testSuiteCount` and `replayCount`.
- Outside an editor session, `Editor --headless --project <P> --export <dir> --config Dist [--testing] [--smoke-test] [--export-report <file>]` (or `python Scripts/Export.py`) exports once and exits 0 exported, 1 failed, 3 the project does not open or a build output is missing.

## Definition of done

A game is done when all of these hold:
1. `project_validate {}` reports no diagnostics and `script_check {}` no findings.
2. `test_run {}` is green: every suite in every mode it lists, and every replay verified with its committed hash. `Testing.Replays` matches at least one committed replay recorded from tick 0 with input events, and every replay with input events fails its verification without them (§7).
3. Every level that a physics ball plays has an autopilot suite and a recorded replay that reaches the goal without it.
4. `project_export {config: "Dist", smokeTest: true}` succeeds from a rendering editor, with `smokeTestRan: true` and `smokeTestExitCode: 0`.
5. `python Scripts/AuditProvenance.py --project <Game>` passes on the staged project (index mode, the acceptance mode: it audits the staged files as a fresh clone holds them): every engine-consumed file has a provenance entry with its current XXH64, written by an automation method whose request at the entry's transcript line in `BuildLog.jsonl` has the same id, method and client (or by `project.upgrade`); no entry names a file that no longer exists unless the transcript records an `asset.delete` or `asset.move` of that file (deleting its folder never excuses it); no two staged paths differ only in letter case.
6. `python Scripts/Test.py --suite games --config Debug,Release,Dist --require-gpu` passes with the game staged: the audit; `script.check`, `project.validate` and the `project.upgrade` dry run clean; every suite and replay at its committed hash in the Debug and Release editors and in a testing Dist export; at least one committed replay with input events, and every one failing its verification without them (`input_replay {verify: true, strictHash: true, withoutInput: true}`); and the plain Dist export's smoke test run and exiting 0.
7. `<Game>/AGENTS.md` describes the scenes, input actions, scripts, suites and replays.

## Pitfalls

- **Arrays replace.** A settings patch with `"Suites": [one entry]` drops every other suite.
- **Unsaved scene.** Recording, replay verification and export use saved files: `scene_save` first.
- **Renderer none.** The bridge's default editor cannot take screenshots (`Unsupported`); relaunch with `renderer: "vulkan"`.
- **Lockstep ownership.** Only the client that started lockstep may step it; a disconnect releases it.
- **Large results** over 48 KB come back as `{path, truncated: true, summary}`: read the file with the bridge's `result_read {path, pointer?, offset?, length?}` (one page of its text with `nextOffset`, or the JSON value at a pointer such as `/cases/0`; `engine_client.load_offloaded` outside the bridge), never with another file tool. `result_read` also reads a test run's `jsonPath` and `junitPath` reports.
- **Hot reload pauses for determinism.** Lockstep, recording, replay and test sessions defer script reloads; ordinary play reloads at safe points.
