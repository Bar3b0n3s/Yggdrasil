# Tetris

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done; `skills/luau-gameplay` and
`skills/game-debugging` cover scripts and diagnosis, and `reference/Components`, `reference/Automation`,
`reference/Validation` and `reference/FileFormats` the engine's data.

- The editor writes every engine-consumed file (`Tetris.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed. `Build/` holds export output
  (`Build/Windows-Dist/Tetris`, `Build/Windows-Dist-Testing/Tetris`); it is not part of the game's sources.
- Tests are Luau suites under `Assets/Tests/` listed in `Testing.Suites`, and recorded replays under `Assets/Tests/Replays/`
  listed in `Testing.Replays`; `test.run` (or `Editor --headless --project <this project> --run-tests`) runs them all.

Built in M17 by the game-builder agent through the MCP tools only (session 1 built it, session 2 fixed the milestone
review's findings G-01 to G-06). Launch it with `editor_launch {project: "Projects/Tetris", renderer: "vulkan"}`
(screenshots and the Dist smoke test need the renderer), then `scene_open {path: "Assets/Scenes/Main.scene"}`.

## Game

A guideline-style Tetris: a 10 x 20 visible well (22 rows; pieces spawn in rows 21 and 22 and drop one row at once), the
7 tetrominoes with SRS rotation and wall kicks (the JLSTZ and I tables), a 7-bag randomizer, gravity by level (a frozen
ticks-per-row table at 60 Hz: 48, 43, 38, ... 1), soft drop (a row every 2 ticks), hard drop, lock delay (30 ticks,
reset by a move or rotation of a grounded piece at most 15 times per row), DAS 10 ticks / ARR 2 ticks, a ghost piece,
hold (once per piece), a three-piece next preview, line clears scoring 100/300/500/800 x level, a level every 10 lines,
and Restart (any time). The game ends three ways: block-out (the next piece's spawn is blocked), lock-out (a piece locks
with every block above the visible rows) and a lock with a block above the well (a rotation kick can lift a piece into
row 23 or higher, which the 22-row grid cannot hold; the lock ends the game instead of dropping that block). HUD texts
show score, level, lines, the last clear ("SINGLE +100" ... "TETRIS! +800", 120 ticks) and "GAME OVER / Press Enter to
restart". Nine synthesized `.sfx` sounds play as one-shots (move, rotate, lock, hard drop, clear, tetris, hold, level up,
game over; none loops).

The look: an orthographic camera (size 12.5) at z = 15 looking down -Z at the well in the XY plane, cell (column c,
row r) at (c - 5.5, r - 10.5, 0); PBR cube cells with HDR emission (`EmissiveStrength` 4 on the seven piece materials,
6 on the `Accent` top line), a directional light with `LightAngle` 10 (soft PCSS shadows on the backboard, one cascade,
`ShadowDistance` 20), GTAO (`SsaoRadius` 1.5, `SsaoIntensity` 2.5: below about 1.2 the occlusion between cells and
backboard almost vanishes at this scale), ACES tonemapping at `ExposureEV` -0.8, `BloomIntensity` 0.3 and the built-in
`engine://Environments/Studio` environment at 0.8.

Bloom has no threshold: it blends `BloomIntensity` of a blurred copy into the image. At session 1's 0.07 with emission
2.2 nothing glowed; raising the piece emission alone to 8 (bloom still 0.07) washed the cells out towards white under
ACES and still showed only a faint halo. Emission 4 with bloom 0.3 gives clearly visible coloured halos around every cell
and the top line while the cells keep their colour (session 2's A/B pair, bloom on and off at the same tick, shows it).
Material contents are not in the state hash; `BloomIntensity` is (it is a scene component), so the replays were
re-recorded. A material's current values: `engine_call {method: "asset.getProperties", params: {asset: <path>}}`.

## Settings

- Window 720 x 900, `Window.Resizable` false: the HUD's screen-space labels sit at fixed fractions of the window's width
  beside the world-space panels, which holds only at this aspect (a maximized window would detach them).
- `Simulation.FixedHz` 60, `Simulation.Seed` 1717 (the session seed decides the piece order: the first bag starts O, J,
  S, Z, I, T, L).
- `StartScene` and `Export.BuildScenes`: `Assets/Scenes/Main.scene`. `Scripting.BlockPlayOnTypeErrors` true.
- Input actions (all Buttons): `MoveLeft` (Left, A, DPadLeft), `MoveRight` (Right, D, DPadRight), `SoftDrop` (Down, S,
  DPadDown), `HardDrop` (Space, W, DPadUp), `RotateCW` (Up, X, South), `RotateCCW` (Z, LeftControl, East), `Hold` (C,
  LeftShift, LeftBumper), `Restart` (Enter, R, Start).

## Scene `Assets/Scenes/Main.scene`

- `/Camera` (orthographic, Primary, Clear Color), `/Sun` (DirectionalLight), `/Environment` (Environment + PostProcess).
- `/Stage`: `Backdrop`, `Backboard` (receives the shadows and GTAO behind the well), `WallLeft`, `WallRight`, `Floor`,
  `TopLine` (emissive accent), `HoldPanel`, `NextPanel`. No physics bodies anywhere.
- `/Game` with the `Game` behaviour; its children `Cells` and `Previews` are empty parents of the pool that `OnStart`
  instantiates from `Assets/Prefabs/Cell.prefab` (200 well cells named `CellRR_CC`, 16 preview cells `PreviewS_I`: slot 1
  is the hold, 2 to 4 the next pieces). The play scene has 241 entities from tick 0 on and never creates or destroys any.
- `/HUD`: Screen `Text` entities `Title`, `HoldLabel`, `NextLabel`, `ScoreText`, `LevelText`, `LinesText`, `ClearText`,
  `StatusText`, `HelpText`. The behaviour finds the five dynamic ones by name and writes a text only when it changes.

## Scripts (`Assets/Scripts/`)

- `Pieces.luau` (Module): shapes in their bounding boxes (Y down), the four rotation states computed by rotating state 0
  clockwise, spawn columns, and the SRS kick tables (`Pieces.Kicks(name, from, to)`, row shifts up-positive).
- `Board.luau` (Module): the 10 x 22 grid as a flat array (`(row - 1) * 10 + column`, row 1 at the bottom); `Fits`
  (rows above the well count as free, so kicks may lift a piece there), `Place` (writes the blocks inside the well and
  returns true on a top-out: every block above the visible rows, or any block above the well), `ClearLines`, `SetRow`
  (test setup: "." empty), `ToAscii` (rows 20 to 1).
- `Bag.luau` (Module): the 7-bag over a `RandomGenerator`.
- `Match.luau` (Module): one game as pure tick-driven rules: `Match.New(generator)`, `Step(controls)` once per tick, and
  `Move`, `Rotate`, `HardDrop`, `HoldPiece`, `SpawnPiece`, `DropDistance`, `GravityTicks`, `Reset`. A lock that `Place`
  reports as a top-out scores its clears, then ends the game without spawning (the queue is not touched). `Events` lists
  what the last step did ("Move", "Rotate", "HardDrop", "Lock", "Clear1".."Clear4", "LevelUp", "Hold", "GameOver",
  "Restart"); the behaviour plays sounds and banners from it. All tuning constants are fields of the module table.
- `Game.luau` (Behaviour on `/Game`): builds the pool in `OnStart`, seeds the match with
  `Random.New(Random.Integer(0, 2147483647))`, and in `OnFixedUpdate` reads the actions, steps the match and mirrors
  the board, ghost, falling piece, previews and HUD into components through shadow arrays (only changes are written).
  Fields: `CellPrefab`, `Materials` (I, O, T, S, Z, J, L, Ghost, other), and the nine sound fields.

Reading the running game (in a lockstep or paused session):

```luau skip="notes"
local game = (Scene.FindByName("Game") :: Entity):GetScript() :: GameScript.Game
return game.Match.Board:ToAscii()
```

## Tests

- `Assets/Tests/Board.test.luau` (suite "Board", empty scene, 19 cases): kicks at both walls (T and I) and the floor (T
  and I), a blocked rotation, clears of 1 to 4 lines and their scores (also at levels 2 and 3), levels every 10 lines,
  the 7-bag, the match's first bag, the three game-over rules, hold once per piece, gravity, soft drop, lock delay and
  its reset, DAS/ARR, hard drop at the ghost position, reset. The game-over cases are built so that only their own rule
  can end the game:
  - "a lock above the visible rows tops out and ends the game" (lock-out): rows 1 to 20 are `.XXXXXXXXX`, an O spawned
    at Top 22 is moved to columns 9 and 10 and hard-dropped (distance 0). It expects GameOver and its event, the O's
    blocks in rows 21 and 22, and no spawn: the O is still the piece at Left 9 and the next queue is unchanged. Without
    the lock-out branch the lock would call `spawnNext`, whose `takeNext` always advances the queue (and the next piece
    fits in the empty spawn columns 4 to 7, so no block-out would end the game either), so the case fails then
    whatever the seed. Session 1's version of this case hard-dropped the seed's first piece onto the filled rows,
    where `Place` returned false and the next spawn's block-out ended the game: it never reached the lock-out branch.
  - "an I kicked above the well tops out when it locks": row 19 `...X.XX...`, an I spawned at Top 21 rotates through
    the (+1, +2) kick to State 1, Left 5, Top 23 and is hard-dropped (distance 0); it expects GameOver, its event, the
    I's blocks in column 7, rows 20 to 22, and no spawn. Before `Place` reported a block above the well this case
    failed (the next I spawned at Left 4 and the queue advanced: the game ended by block-out, a block was silently
    dropped).
  - "a blocked spawn tops out and ends the game" (block-out).
- `Assets/Tests/Tetris.test.luau` (suite "Tetris", `Main.scene`, `Isolation: "Case"`, 11 cases driven by
  `Test.InjectAction`): pool and HUD at start, hard drop, taps and DAS to the wall, both rotations, hold and its preview,
  soft drop, the ghost (exactly 4 ghost cells before and after a MoveLeft tap, each at the falling piece's column in its
  landing row `Top - DropDistance()`), a line clear on the HUD, a tetris, a level rise, top-out by stacked hard drops
  then Restart. Some cases set the board up through `game.Match` first (`SetRow`, `SpawnPiece`).
- Recording suites (`Modes: ["Editor"]`, `Main.scene`): `Assets/Tests/Record/Opening.test.luau` ("Record.Opening",
  a planned tap list) and `Assets/Tests/Record/LineClear.test.luau` ("Record.LineClear", an autopilot from
  `Assets/Tests/Lib/Autopilot.luau` that scores every resting place by lines, aggregate height, holes and bumpiness,
  then steers with taps and a hard drop until a line clears).

## Replays (`Assets/Tests/Replays/`)

| Replay | Recorded with | Ticks | Events | Checkpoints | FinalStateHash | Without input |
|---|---|---|---|---|---|---|
| `Opening.replay` | `test_run {filter: "Record.Opening", record: ...}` | 301 | 44 | 5 | `5b96177faa088fd0` | fails at checkpoint tick 60 |
| `LineClear.replay` | `test_run {filter: "Record.LineClear", record: ...}` | 95 | 34 | 1 | `18c6b54ec7f80aff` | fails at checkpoint tick 60 |

Opening ends with O, J, Z, S and T locked (the Z after a 60-tick soft drop), the I in the hold and L falling; LineClear
ends with one line cleared, score 100 and "SINGLE +100" on the HUD. Session 2 re-recorded both after raising
`PostProcess.BloomIntensity` from 0.07 to 0.3 (session 1's hashes were `bcac10e522808880` and `39f647ca007f08f6`; the
script fixes of session 2 left those unchanged, checked with `test_run {}` before the bloom change). M20 re-recorded both
the same way after its engine changes (Script component version 2 with ordered script slots, PrefabInstance version 2,
the PrefabLink key `OuterPrefabEntityID`) changed the hashed components; session 2's hashes were `93ad9cdf7978c2d2`
and `bd018732d506bb4e`, and the ticks, events and checkpoint counts above are unchanged. Each was verified twice with `input_replay {verify: true, strictHash:
true}` (equal hashes) and fails without its input (with `continuePastCheckpoint`, every checkpoint differs).
**The state hash covers every serialized component, the look included**: changing a `PostProcess`, light or camera
value in `Main.scene`, or anything the behaviour writes, changes both hashes, so re-record both replays
(`test_run {filter, record}`), verify each twice and run the input check again after any such change. Material asset
contents are not hashed.

## Status (end of session 2)

Done: every Roadmap M17 feature and the review findings G-01 to G-05 (G-06: `Board.IsFree` removed; `Pieces`' unused
`Shape.Name` and `Shape.Size` kept, since `Pieces.luau` was not otherwise edited); `project_validate` clean;
`script_check` no diagnostics; `project_upgrade {dryRun}` nothing to rewrite; `test_run {}` 34/34 green (19 Board, 11
Tetris, 2 recording, 2 replays); soak from the saved scene without input to tick 10,000: no script error, 241 entities,
0 bodies, 0 voices at ticks 5,100 and 9,600 (script heap 1.33 MB, 1.43 MB, then 1.19 MB at 10,000: collection timing,
no growth; the game tops out before tick 10,000 and then waits for Restart); Dist export with smoke test (ran, exit 0).

Not verifiable from automation: the games suite (Debug and Release editors and the testing Dist export's
`--feature-test` run comparing the replay hashes) and `Scripts/AuditProvenance.py`; run them from the repository.

## Next steps

- Run `python Scripts/Test.py --suite games --config Debug,Release,Dist --require-gpu` and the provenance audit with the
  project staged (keep `Build/` and `Library/` out of the commit).
- Ideas left out: T-spin scoring, soft and hard drop points, a line-clear flash, a pause action.
