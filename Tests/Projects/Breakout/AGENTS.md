# Breakout

Notes for agents and people working on this game (the M16 dry-run game). The engine's own guide is the engine repository's
`AGENTS.md`; the editor serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done; `skills/luau-gameplay` and
`skills/game-debugging` cover scripts and diagnosis, and `reference/Components`, `reference/ScriptAPI`,
`reference/Automation`, `reference/Validation` and `reference/FileFormats` the engine's data. The long reference topics come
back as a first page with a section list: ask for one section (`{topic: "reference/ScriptAPI", section: "RigidBody"}`,
`{topic: "reference/Automation", section: "play.step"}`) or a table of contents (`toc: true`) instead of reading them whole.
The typed tools take every parameter the current catalogue declares: a client whose tool list predates a catalogue change
sends a parameter its cached schema lacks as a string, and the bridge converts a string that is exactly a JSON literal of
the declared type (`docs_get {toc: "true"}` reaches the editor as `toc: true`). Start a fresh Claude Code session (or
`/mcp` Reconnect) after a catalogue change to see the current schemas; `engine_call` stays the escape hatch.

- The editor writes every engine-consumed file (`Breakout.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand. The transcript is append-only: its session 1
  lines still name absolute paths (the `project.create` at line 1, offloaded-result paths in early response summaries),
  written by the bridge before it recorded repository-relative paths, and they stay as written.
- `Library/` is the editor's cache, autosave, trash and lock, and `Build/` holds exports; neither is committed.
- Open it with `editor_launch {project: "Tests/Projects/Breakout", renderer: "vulkan"}` (a relative path names a directory
  below the repository root; vulkan for screenshots and the Dist smoke test), or, when an editor already has the project
  open with automation allowed, `editor_attach {project: ...}`. Both report the editor's `session`, including the open
  scene; a launched or attached editor may have none open (`session.scene: null`): `scene_open {path:
  "Assets/Scenes/Main.scene"}` before `entity_get` or `scene_tree`.
- After an engine change that alters canonical output, run `project_upgrade {dryRun: true}`, then `project_upgrade {}`;
  `changes` names each rewritten file with its reason, and the dry run must list no files afterwards.

## The game

A 2D Breakout in the XY plane, seen through an orthographic camera looking down -Z. The paddle moves along x at y = -6.5,
the ball is served resting on it and launched upwards; walls bound x = ±8.5 and the top at y = 7; the ball is lost below
y = -8.5. Five rows of ten bricks score 5, 4, 3, 2 and 1 points from the top row down. Three lives; game over at zero,
a win when every brick is gone; Restart starts a new game in place (it reactivates the bricks; it does not reload the scene).

The ball leaves the paddle at an angle set by where it hits (up to 60 degrees from vertical at the edges). It is served at
9 m/s and speeds up by 1 m/s on every 4th paddle hit of a rally, up to 13 m/s; a rally ends when the ball is lost, the game
is won or restarted, so every serve starts at 9 m/s again.

## Settings

- `StartScene` and `Export.BuildScenes`: `Assets/Scenes/Main.scene`. `Simulation.Seed` 1977 at 60 Hz. Window 1200 x 900.
- `Scripting.BlockPlayOnTypeErrors` is on.
- Input actions:
  - `MoveX` (Axis): Right/D/DPadRight positive, Left/A/DPadLeft negative, gamepad left stick X.
  - `Launch` (Button): Space, Up, gamepad South.
  - `Restart` (Button): R, Enter, gamepad Start.

## Scene `Assets/Scenes/Main.scene`

- `Camera` (orthographic, size 8, at z = 20, Primary, AudioListener), `Sun` (directional, no shadows), `Environment`
  (fallback ambient, PostProcess with PbrNeutral tone mapping, no SSAO).
- `Arena` (tag `Wall`): one Static RigidBody (friction 0, restitution 1) whose children `LeftWall`, `RightWall` and
  `TopWall` are scaled cubes with BoxColliders, so the walls are one compound body.
- `Paddle` (tag `Paddle`): Kinematic body moved with `MoveKinematic`; scale 2.4 x 0.4.
- `Ball` (tag `Ball`): Dynamic sphere, radius 0.25, gravity factor 0, friction 0, restitution 1, no damping, LinearCast,
  z translation and every rotation locked, MaxLinearVelocity 500; `Ball.luau` with `Game` pointing at the Game entity.
- `Bricks`: 50 instances of `Assets/Prefabs/Brick.prefab` named `Brick_R<row>_C<column>` (row 1 at the top, y = 5.5,
  rows 0.6 apart, columns 1.6 apart from x = -7.2). Each row overrides the material (`BrickRed`, `BrickOrange`,
  `BrickYellow`, `BrickGreen`, the prefab's `BrickBlue`) and `Scripts/0/Fields` (`Points`). Bricks are tagged `Brick`
  and have their own Static bodies.
- `ScoreText`, `LivesText`, `MessageText`: screen-space Text (anchors are top-left based).
- `Game`: `Game.luau`, whose fields reference the entities above and the six sounds. The speed fields (`BallSpeed`,
  `SpeedUpHits`, `SpeedStep`, `MaxBallSpeed`) are not overridden in the scene, so the script defaults (9, 4, 1, 13) apply.

## Scripts (`Assets/Scripts/`)

- `Rules.luau` (Module): the pure rules (state with Score, Lives, BricksLeft, Phase `Serve | Playing | Won | GameOver` and
  RallyHits, the paddle hits since the last serve; Launch, PaddleHit, BreakBrick, LoseBall, Restart, Message,
  BounceDirection, FixDirection, and BallSpeed, which turns a `SpeedRule {Base, HitsPerStep, Step, Max}` and the rally's
  hits into the ball's speed, never above Max nor below Base). No engine calls.
- `Game.luau` (Behaviour): input, paddle movement, serving and launching, the ball speed (`CurrentSpeed()`, re-applied
  every fixed step with a minimum vertical share; a paddle hit is counted before its bounce, so the speed-up applies to
  that bounce), paddle steering by hit offset (`MaxBounceAngle`, 60 degrees), brick breaking (`SetActive(false)`), lives,
  HUD and `Audio.PlayOneShot` sounds. Counters `PaddleHits`, `WallHits`, `BricksBroken` and `LivesLost` count the whole
  session (Restart keeps them); `State.RallyHits` counts the current rally.
- `Ball.luau` (Behaviour): forwards `OnCollisionEnter` to `Game:OnBallHit`.
- `Brick.luau` (Behaviour): only the `Points` field.

## Assets

- `Assets/Materials/`: five brick colours, `Paddle`, `Ball`, `Wall`.
- `Assets/Audio/` (synthesized `.sfx`): `PaddleHit`, `WallHit`, `BrickBreak`, `LifeLost`, `Win`, `GameOver`.
- `Assets/Prefabs/Brick.prefab`.

## Tests

- `Assets/Tests/Rules.test.luau` (suite `Rules`, empty scene, 12 cases): the pure rules, including the rally count and
  the speed-up with its cap.
- `Assets/Tests/Breakout.test.luau` (suite `Breakout`, Main scene, `Isolation: Case`, 13 cases): serving, the ball's speed
  between speed-ups, paddle limits, real brick hits, per-row points, losing a life, game over, winning, restart after game
  over, after a win and during play (score, lives, bricks, HUD, paddle and ball reset; the session counters keep
  counting), a speed-up after `SpeedUpHits` real paddle hits (and back to the serve speed after a lost ball), and the top
  speed (that case sets `SpeedUpHits = 1` and `MaxBallSpeed` two steps up as tuning, then checks a restart serves at the
  base speed). Cases may teleport the ball below the paddle as setup (`dropBall`); the speed cases return the ball with
  injected `MoveX` only. Restarts are covered here, not by the autopilot.
- `Assets/Tests/Autopilot/Breakout.test.luau` (suite `Autopilot.Breakout`, `Modes: ["Editor"]`, case "hits, speeds up,
  breaks bricks and loses a life"): reads the scene and injects only `MoveX` and `Launch`. It rallies until the rally has
  `SpeedUpHits` paddle hits and at least 3 bricks broke (asserting the ball sped up), lets the ball fall to lose a life,
  serves again, returns the ball once more and ends 20 ticks later with the ball in play. Its last assertions (run only
  when the suite runs, not when the replay is verified): exactly one lost life, phase `Playing`, a positive score, the
  broken bricks inactive in the scene and the HUD texts matching the state. It recorded the replay.
- `Assets/Tests/Replays/Opening.replay` (format version 3, 872 ticks from tick 0, 791 input events, 14 checkpoints at
  ticks 60 to 840, final hash `685d68832c1f74d4`, re-recorded in M20; the editor's input check accepted it, diverging at
  tick 60). It has no `Expect`
  entries (the `test_run` record path takes none), so what its verification proves is limited to what the state hash
  covers: playing its recorded `MoveX`/`Launch` input without the autopilot reproduces the recorded world at every
  checkpoint and at the end, which pins every entity's active flag (8 bricks inactive at the end), the HUD Text
  components (`SCORE 12`, `LIVES 2`) and every body's position and velocity. It does not assert script-table values;
  replayed in session 3 and again after M20's re-recording and inspected with `script_eval` at its last tick, the game
  showed phase `Playing`, score 12, 2 lives, 42 bricks left, 5 paddle hits, 4 wall hits, 8 broken bricks, 1 lost life and
  `RallyHits` 1. Without its input (`input_replay {verify: true, strictHash: true, withoutInput: true}`) it fails at its
  first checkpoint (tick 60: expected `84971d0672aca529`, observed `66a11366c8454909`; with `continuePastCheckpoint` all
  14 checkpoints differ and the final hash is `7afe459503c0156c`). Re-record with
  `test_run {filter: "Autopilot.Breakout", record: "Assets/Tests/Replays/Opening.replay"}` after a legitimate simulation
  change.
- Earlier recordings: session 1's was 636 ticks, `3a109217817a6b09`; session 2's was format version 1 (no checkpoints),
  902 ticks, `7bca7f325ca12227`; session 3's was format version 2 with the same 872 ticks and 14 checkpoints,
  `d4b7ace57c8d7e15`, until M20's engine changes (Script component version 2 with ordered script slots, PrefabInstance
  version 2, the PrefabLink key `OuterPrefabEntityID`, the pending-enter rule for physics contacts) changed the state
  hash. Session 2's ended with a restart in place, and `7bca7f325ca12227` is the hash of the untouched start at tick
  902, so it also passed without its input and proved nothing about the gameplay. Keep the played state in the scene at
  the end of a recording.
- The state hash does not cover values held only in script tables (such as `State.RallyHits`): a change that only alters
  script state leaves replay hashes unchanged, so cover such state with test cases.
- `test_run {}` runs 27 rows (25 cases, the autopilot case and the replay); all pass.

## Status after builder session 3

- `project.upgrade` (dry run, then applied) rewrote one file, `Assets/Tests/Replays/Opening.replay` (version 1 to 2,
  41 unchanged); the replay was then re-recorded, and the dry run now lists no files (42 unchanged).
- `project_validate` and `script_check` are clean; `test_run {}` passes 27 of 27 rows; `input_replay {verify: true,
  strictHash: true}` matches `d4b7ace57c8d7e15` with 14 checkpoints, and the same with `withoutInput: true` added fails at
  checkpoint tick 60.
- `project_export {config: "Dist", smokeTest: true}` succeeds with `smokeTestRan: true`, exit code 0 and no warnings; a
  testing export packages 3 suites and 1 replay.
- Session 3 also recorded a probe, `Assets/Tests/Probe/RestartEnding.replay` (the autopilot with a restart appended), to
  check the input check; it was accepted and then moved to `Library/Trash` with `asset_delete`. The transcript records
  both; no probe asset remains.

## Status at the M16 milestone review

- The provenance audit passes in index mode on the staged project (`python Scripts/AuditProvenance.py --project
  Tests/Projects/Breakout`: 50 engine-consumed files, 50 entries, 392 transcript lines with 196 requests, no finding or
  warning).
- The games suite passes (`python Scripts/Test.py --suite games --config Debug,Release,Dist --require-gpu`): the audit;
  `script.check`, `project.validate` and the `project.upgrade` dry run clean in the strict Debug and Release editors,
  whose `test.run` passes 27 rows each with `Opening.replay` at `d4b7ace57c8d7e15`; `Opening.replay`, the committed replay
  with input events (791), fails its verification without them; the testing Dist export's `--feature-test` passes 26 rows
  (the Editor-only autopilot suite left out) with the same hash; the plain Dist export's smoke test runs and exits 0.
