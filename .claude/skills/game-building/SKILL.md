---
name: game-building
description: Build a game end to end through the editor's automation (the MCP tools or engine_client): launch, project settings, scenes, prefabs, scripts, deterministic checks, test suites, recorded replays with the autopilot workflow, validation and export, and the provenance, transcript and demo-audit rules that define done.
---

# Game building

This is the workflow an agent follows to build a game with this engine, from an empty project to an exported build. Architecture §13 is the authority (§13.11 walks through Tetris; §13.12 is the demo acceptance rule). The method reference is `docs.get {topic: "reference/Automation"}` (full schemas in `Docs/Reference/Methods.json`); components, file formats and validation codes are `reference/Components`, `reference/FileFormats` and `reference/Validation`. Write gameplay with the `luau-gameplay` skill, and diagnose with `game-debugging`. Tools are named `domain_verb` as the MCP bridge proxies them; a method without a tool (`prefab.revert`, `project.upgrade`, `play.state`) is reached through `engine_call {method, params}`, and `engine_client` calls every method by its `domain.verb` name.

## Ground rules

- **Only through automation.** Every engine-consumed file of a game (`<Game>.eproj`, everything under `Assets/` and every `.meta`) is written by the editor through an automation method: never with a text editor, a shell or a file-copy tool. `Scripts/AuditProvenance.py` rejects any other origin (§13.12).
- **Provenance and transcript are committed.** The editor records each write in `Projects/<Game>/Automation/Provenance.json` (`{Path, XXH64, Method, RequestId, Client, TranscriptLine}`), and the MCP bridge appends every request to `Projects/<Game>/Automation/BuildLog.jsonl`. Never edit either by hand; never delete the transcript.
- **Windowed UI writes do not count.** A file saved from the editor's own UI is recorded with method `ui`, which the audit rejects for demo games. Use the UI to look, not to author.
- **`Library/` is a cache** (import cache, autosave, lock, offloaded results). It is gitignored and never consulted by the audit.
- **Documentation is exempt.** `Projects/<Game>/AGENTS.md` and a `README.md` may be written directly; describe the game's scenes, actions, scripts and tests there when you finish.
- **A missing capability is an engine change.** When automation cannot do what the game needs, add the method (the `add-automation-method` skill) or the script API (`add-script-api`) with its tests before continuing. Never work around it by hand.

## 1. Launch

- `editor_launch {project: "Projects/<Game>", create: true, template: "Empty"}` creates and opens the project (templates: `Empty`, `Basic3D`). Without `create` it opens an existing one. The bridge launches the Release editor headless with `renderer: "none"`; pass `renderer: "vulkan"` when you need screenshots.
- If the user already has the project open with "Allow AI automation" on, `editor_launch` attaches to that editor instead (§13.8). `EditorAlreadyOpen {pid}` means an editor without automation holds the project: ask the user to allow automation, then `editor_attach`.
- After `EditorCrashed {autosaveAvailable: true}`, `editor_launch {recover: true}` adopts the autosave as an unsaved scene; save it deliberately.
- `docs_get {topic: "skills/game-building"}` (this file) and the other skills are served by the editor.

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

- `project_get_settings` returns the whole document; `reference/Components` and `Schemas/Project.schema.json` list every field.
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
- `asset_create {type: Material | SoundEffect | Scene | Prefab | Folder, path, values}` writes native assets; `asset_import {source, destDir}` imports external files (glTF, PNG, HDR, audio, fonts).
- Prefabs: build an entity, `prefab_create {entity, path, replaceWithInstance?}`, then `prefab_instantiate {prefab, parent, transform}`; `prefab_apply` writes an instance's overrides into the prefab and `prefab.revert` (through `engine_call`) drops them.
- **Tracks and levels.** Give each level a root entity with a `Static` `RigidBody` and instantiate every track piece under it: the pieces join one static compound body, so a rolling ball crosses seams without bumps. Separate static bodies that touch are reported as `PHYSICS_ADJACENT_STATIC_BODIES`, and `project_validate {fix: ["PHYSICS_ADJACENT_STATIC_BODIES"]}` adds that root body for you. Lay out a level as one `edit_batch` of `prefab.instantiate` ops chained with `$ref`, plus ordered waypoint entities along the main route for the autopilot.
- Look at the result: `scene_tree {format: "text"}` for the outline, `entity_bounds` for layout and gaps, and `viewport_screenshot {view: "game", annotate: {labels: "all"}}` (needs `renderer: "vulkan"`).

## 4. Scripts

- `script_create {path, template: "Behaviour" | "Module" | "Test"}`, then `script_write {path, source}`. Each result carries compile and type diagnostics with full ranges; iterate until `script_check {}` reports none. Set `Scripting.BlockPlayOnTypeErrors` to keep type errors out of play: play, reloads and test runs then refuse them (test runs always do).
- Attach a behaviour with `entity_update {entity, components: {Script: {Script: "Assets/Scripts/Game.luau", Fields: {...}}}}`. `_meta` and `project_validate` report overrides that match no declared field (`SCRIPT_UNKNOWN_FIELD_OVERRIDE`) or have the wrong type (`SCRIPT_FIELD_TYPE_MISMATCH`).
- Keep pure logic (boards, scoring, kick tables) in Modules without engine calls, so the empty-scene suites can test it.

## 5. Verify deterministically

- `play_start {lockstep: true, seed: 42}` decouples simulation from the wall clock; only `play_step {ticks, input?, render?}` advances it. Each result carries `tick` and `stateHash`.
- Input events are tick offsets from the step's start: `{tick: 0, type: "action", name: "MoveLeft", state: "tap"}` (down this tick, up the next). Analog actions take `value`.
- `script_eval {context: "play", entity: "/Game", code: "return self.Board:ToAscii()"}` reads state; `play_wait_for {until: "<luau returning a boolean>", timeoutTicks}` steps until a predicate holds.
- Repeated runs with the same seed and input must give the same `stateHash`. A difference is a determinism bug in the game: see the pitfalls in `luau-gameplay`.
- `play_stop` returns to the edit scene unchanged.

## 6. Test suites

- One suite per area: pure-logic suites in the empty scene (`Board.test.luau`) and integration suites bound to a scene (`Tetris.test.luau`) that drive play with `Test.InjectAction`, `Test.WaitTicks`, `Test.WaitUntil` and `Test.GetStateHash`.
- `test_run {}` runs every suite and verifies every replay; `test_run {filter: "Tetris/hard drop"}` selects by `<suite>/<case>` substring. Results list each case with its outcome, failures with file and line, and the JUnit path. A green run has no failed, errored, timed-out or skipped rows.
- The same suites run headless with `Editor --headless --project Projects/<Game> --run-tests [--filter F]` and, in exported testing builds, with `Runtime --feature-test`.

## 7. Replays

A replay records the input applied per tick from tick 0 and the final state hash (§6.7, §13.6).

- Save the scene first: a recording plays the saved scene asset its header names. In Edit mode, `input_record {action: "start", scene?, parameters?, seed?}` starts a lockstep session at tick 0 and records. Drive it with `play_step` input, then `input_record {action: "stop", path: "Assets/Tests/Replays/Opening.replay", expect: [{tick: 600, luau: "return Scene.FindByName('Game') ~= nil"}]}`.
- A hot reload or a play-scene edit during the recording invalidates it: `stop` then fails, writes nothing and names the reason.
- Verify with `input_replay {path, verify: true, strictHash: true}`: every `Expect` must hold and the final hash must match. `Testing.Replays` makes every test run verify them.

## 8. The autopilot-and-record workflow

Steering a physics ball with hand-written tick-stamped input is impractical. Instead write a test-only **autopilot suite** that steers like a player, record its run, and verify the replay without it:

1. Add `Assets/Tests/Autopilot/Level1.test.luau` to `Testing.Suites` bound to the level, with `Modes: ["Editor"]`, so exported builds never run it.
2. Its single case reads the ball's state, follows the level's waypoint entities with a PD controller, and injects analog actions with `Test.InjectAction` every tick until the goal is reached. It must only read the scene and inject input: never move entities, write components or draw from the session's `Random` stream, so the run equals a player pressing the same inputs.
3. Record it: `test_run {filter: "Autopilot.Level1", record: "Assets/Tests/Replays/Level1.replay"}`. The filter must select exactly one suite. The editor verifies the candidate in fresh ordinary gameplay without the test driver, final hash included, before it writes the file.
4. The replay is then verified **without** the autopilot by `input_replay {verify: true, strictHash: true}` and by every test run through `Testing.Replays`, which proves that plain input reaches the goal. Level 2 and 3 suites set `Parameters` (such as the carried timer), which the replay header stores.

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
- The review of the change states why the hashes changed. A verifier's hash never replaces a recorded one by hand.
- An engine change that alters the canonical form of project files also runs `project.upgrade` on every affected game (`Editor --headless --project Projects/<Game> --upgrade` appends its own transcript line, client `cli`), so the audit and the byte-identical re-save test stay green.

## 10. Validate and export

- `project_validate {}` must report nothing. `fix: true` applies every safe fix as one undoable command; `fix: ["<id or CODE>"]` only the selected ones. `reference/Validation` explains each code and its fix.
- `project_export {config: "Dist", smokeTest: true}` writes `Build/<Platform>-Dist/<Game>/` and runs the exported executable with `--headless --frames 300 --expect-no-errors`; it must exit 0. Export refuses script type errors, asset errors (also of content `Export.Exclude` leaves out, `Assets/Tests/**` included), missing build scenes, an `Export.Version` that is not `major.minor.patch` (each part at most 65535), an `Export.Icon` that is not a square PNG of at least 256 × 256, and an existing output directory that holds anything but an earlier export. On Windows the executable carries the icon, version and company (`resourcesPatched`); every package holds `THIRD_PARTY_NOTICES.txt`, and a macOS export is `<Game>.app`. The result's `phaseTimings` show where the time went.
- `project_export {config: "Dist", testing: true}` is a testing export (`Build/<Platform>-Dist-Testing/<Game>/`): it packages `Assets/Tests/**`, every `Testing.Suites` script and scene and every `Testing.Replays` match whatever `Export.Exclude` says, refuses invalid suites and replays whose expectations do not compile (issues at `/Testing/Suites/<i>` and `/Testing/Replays/<i>`), and its `Game.json` says `"Testing": true`, so `<Game> --headless --feature-test` runs the suites and replays in the shipping binary and writes `bin/TestResults/FeatureTest-<Mode>.{json,xml}` below its working directory, its user data in `<package>/bin/TestUserData/<Game>`. The result counts `testSuiteCount` and `replayCount`.
- Outside an editor session, `Editor --headless --project <P> --export <dir> --config Dist [--testing] [--smoke-test] [--export-report <file>]` (or `python Scripts/Export.py`) exports once and exits 0 exported, 1 failed, 3 the project does not open or a build output is missing.

## Definition of done

A game is done when all of these hold:
1. `project_validate {}` reports no diagnostics and `script_check {}` no findings.
2. `test_run {}` is green: every suite in every mode it lists, and every replay verified with its committed hash.
3. Every level that a physics ball plays has an autopilot suite and a recorded replay that reaches the goal without it.
4. `project_export {config: "Dist", smokeTest: true}` succeeds.
5. `Scripts/AuditProvenance.py` passes: every engine-consumed file has a provenance entry with its current XXH64, written by an automation method whose request id appears at its transcript line in `BuildLog.jsonl` (or by `project.upgrade`); no entry names a file that no longer exists unless the transcript records its deletion or move.
6. `Projects/<Game>/AGENTS.md` describes the scenes, input actions, scripts, suites and replays.

## Pitfalls

- **Arrays replace.** A settings patch with `"Suites": [one entry]` drops every other suite.
- **Unsaved scene.** Recording, replay verification and export use saved files: `scene_save` first.
- **Renderer none.** The bridge's default editor cannot take screenshots (`Unsupported`); relaunch with `renderer: "vulkan"`.
- **Lockstep ownership.** Only the client that started lockstep may step it; a disconnect releases it.
- **Large results** over 48 KB come back as `{path, truncated: true, summary}`: read the file the path names.
- **Hot reload pauses for determinism.** Lockstep, recording, replay and test sessions defer script reloads; ordinary play reloads at safe points.
