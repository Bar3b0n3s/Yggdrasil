# Tetris

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done, and its pages
`skills/game-building/sprites-2d` and `skills/game-building/ui`; `skills/luau-gameplay` and `skills/game-debugging` cover
scripts and diagnosis, and `reference/Components`, `reference/Automation`, `reference/Validation` and
`reference/FileFormats` the engine's data.

- The editor writes every engine-consumed file (`Tetris.eproj`, `.luaurc`, `Assets/**` and their `.meta` files)
  through automation, and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends
  every request to the committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave, lock and kept screenshots; it is not committed. `Build/` holds export
  output (`Build/Windows-Dist/Tetris`, `Build/Windows-Dist-Testing/Tetris`); it is not part of the game's sources.
- Tests are Luau suites under `Assets/Tests/` listed in `Testing.Suites`, and recorded replays under `Assets/Tests/Replays/`
  listed in `Testing.Replays`; `test.run` (or `Editor --headless --project <this project> --run-tests`) runs them all.

Rebuilt in place as pixel art in M25 (`Docs/Decisions/0036-m25-tetris-pixel-art.md` in the engine repository) by the
builder agent through the MCP tools only, from an emptied folder: session S1 created the project with
`editor_launch {project: "Projects/Tetris", create: true, template: "Empty", renderer: "vulkan"}`, session S2
continued it with `editor_launch {project: "Projects/Tetris", renderer: "vulkan"}`, and session S3 (two test-suite
review findings only) with `editor_launch {project: "Projects/Tetris", renderer: "none"}`. M17's 3D build (cube
cells with PBR materials, shadows, GTAO and bloom, a Screen `Text` HUD) is history in git, not guidance. Launch it with
`editor_launch {project: "Projects/Tetris", renderer: "vulkan"}` (screenshots and the Dist smoke test need the renderer),
then `scene_open {path: "Assets/Scenes/Main.scene"}`.

## Game

M17's feature list on 2D sprites and UI, unchanged in its rules: a 10 x 20 visible well (22 rows; pieces spawn in rows
21 and 22 and drop one row at once; rows 21 and 22 are never drawn), the 7 tetrominoes with SRS rotation and the JLSTZ
and I wall-kick tables, a 7-bag randomizer, gravity by level (a frozen ticks-per-row table at 60 Hz: 48, 43, 38, ... 1),
soft drop (a row every 2 ticks), hard drop, lock delay (30 ticks, reset by a move or rotation of a grounded piece at most
15 times per row), DAS 10 ticks / ARR 2 ticks, a ghost piece, hold (once per piece), a three-piece next preview, line
clears scoring 100/300/500/800 x level, a level every 10 lines, game over (block-out, lock-out, or a lock with a block
above the well) and restart at any time. HUD: score, level and lines, the last clear ("SINGLE +100" ... "TETRIS! +800",
120 ticks), the title, HOLD, NEXT, the controls help and a game-over panel with "GAME OVER", "PRESS ENTER" and a RESTART
button. Nine synthesized one-shot `.sfx` sounds (move, rotate, lock, hard drop, clear, tetris, hold, level up, game
over; none loops). Left out, as in M17: T-spins, drop points, a line-clear flash and pause.

The board is 200 `SpriteRenderer` cells under a `PixelPerfectCamera`, never a `MeshRenderer` or a Tilemap; the HUD is
UI (`UiText`, `UiImage`, `UiButton`) in one Screen canvas, never a `Text` component. There is no light, `Light2D`,
`Environment`, `PostProcess`, mesh or physics body anywhere.

## Settings

- `Name` Tetris; `StartScene` and `Export.BuildScenes`: `Assets/Scenes/Main.scene` (`Export.Exclude` `Assets/Tests/**`,
  `Version` 2.0.0, `Company` Engine Demos, no icon).
- Window 720 x 900, `VSync` true, not fullscreen, `Resizable` false: every deterministic session (lockstep, test runs,
  recordings, replays, the Dist `--feature-test`) runs at 720 x 900, which shows the 360 x 450 reference resolution
  doubled with no letterbox bars, so the output scale and the pointer mapping are the same everywhere.
- `Simulation`: `FixedHz` 60, `MaxStepsPerFrame` 5, `Seed` 1717 (the session seed decides the piece order: the first bag
  starts O, J, S, Z, I, T, L), `MaxEntities` 65536.
- Input actions (all Buttons): `MoveLeft` (Left, A, DPadLeft), `MoveRight` (Right, D, DPadRight), `SoftDrop` (Down, S,
  DPadDown), `HardDrop` (Space, W, DPadUp), `RotateCW` (Up, X, South), `RotateCCW` (Z, LeftControl, East), `Hold` (C,
  LeftShift, LeftBumper), `Restart` (Enter, R, Start).
- `Scripting`: `MemoryLimitMB` 256, `CallbackBudgetMs` 1000, `PauseOnError` true, `BlockPlayOnTypeErrors` true.
- `SortingLayers`: `Default`, `Board` (the cells), `Frame` (the bars).
- `Ui.Navigation` false (the other `Ui` members keep their defaults): the game's arrows, Space, Enter and the gamepad's
  South and East are also the UI's default Submit, Cancel and direction bindings, and UI input consumes no key. The HUD
  canvas has no `DefaultFocus` and the RESTART button is not `Focusable`, so only the pointer clicks it.

## Scene `Assets/Scenes/Main.scene`

- `/Camera`: `Transform` at (0, 0, 10) looking down -Z; `Camera` Orthographic, Primary, `Clear` Color (the Background
  colour); `PixelPerfectCamera {ReferenceWidth: 360, ReferenceHeight: 450, PixelsPerUnit: 16, LetterboxColor:
  Background}`. One unit is 16 scene pixels and 32 screen pixels; scene pixel (x, y), y down, is world
  (x / 16 - 11.25, 14.0625 - y / 16).
- `/Frame` (`SortingLayer` `Frame`, the built-in white square: a null `Texture`): `WallLeft` (-5.125, -0.125) and
  `WallRight` (5.125, -0.125) scaled (0.25, 20.25), `Floor` (0, -10.125) scaled (10.5, 0.25), all in the Frame colour,
  and `TopLine` (0, 10.0625) scaled (10, 0.125) in the Accent colour, marking the top of the visible well.
- `/Game` with the `Game` behaviour (fields `CellPrefab` = `Assets/Prefabs/Cell.prefab` and the nine sounds).
  - `/Game/Cells`: empty in the file; `OnStart` instantiates 200 cells from `Assets/Prefabs/Cell.prefab`
    (`SpriteRenderer {Texture: engine://Textures/SpriteBevel, PixelsPerUnit: 16, SortingLayer: Board, Color: Empty}`)
    at (column - 5.5, row - 10.5, 0), named `CellRR_CC` (row 1 at the bottom). A cell is never hidden: the behaviour
    writes its `Color` (empty, ghost, a piece's colour, or "other") only when it changes. The well's cells occupy scene
    pixels x 100-260 by y 65-385 (screen 200-520 by 130-770).
  - `/Game/HUD`: `UiCanvas {ScaleMode: ConstantPixelSize, ScaleFactor: 1, PixelSnap: true, RenderMode: Screen}`, one
    canvas unit per reference pixel, the default font. It sits below `/Game`, so a click on RESTART bubbles to the `Game`
    behaviour's `OnUiClick`; no behaviour sits on a HUD entity. Every rectangle is anchored at its parent's top-left
    corner (`AnchorMin = AnchorMax = Pivot = [0, 0]`; `Position` the top-left corner, `Size` its size); every `UiText`
    has `Wrap` false and `RaycastTarget` false, and every `UiImage` but the button's has `RaycastTarget` false. In draw
    order (position, size): `Title` (100, 16, 160 x 32), `HoldLabel` (8, 65, 80 x 13), `HoldBox` (8, 79, 80 x 48;
    preview cells `Preview1_1..4`), `ScoreLabel`/`ScoreText` (8, 141, 80 x 13)/(8, 155, 80 x 17),
    `LevelLabel`/`LevelText` (8, 181, 80 x 13)/(8, 195, 80 x 17), `LinesLabel`/`LinesText` (8, 221, 80 x 13)/(8, 235,
    80 x 17), `ClearText` (8, 275, 80 x 32), `NextLabel` (272, 65, 80 x 13), `NextBox1..3` (272, 79/131/183, 80 x 48;
    preview cells `Preview2_*`, `Preview3_*`, `Preview4_*`), `HelpText` (8, 397, 344 x 48) and `GameOverPanel` (108, 185,
    144 x 80, inactive until game over) holding `GameOverTitle` (0, 8, 144 x 20), `GameOverHint` (0, 30, 144 x 12) and
    `RestartButton` (32, 50, 80 x 20; `UiImage` Sliced + `UiButton {Interactable: true, Focusable: false}`) with its
    stretched `RestartLabel`. The boxes, the panel and the button are 9-sliced `Assets/Sheets/Blocks.spritesheet` frame
    0 (the bevel, 2-texel borders); the 16 preview cells are 16 x 16 `UiImage`s of the same frame, inactive in the file,
    whose `UiRect.Position` and `UiImage.Color` the behaviour writes when their slot's piece changes (centred: I at
    (8, 16), O at (24, 8), the 3-wide pieces at (16, 8)); the hold slot's cells are inactive while the hold is empty.
- The play scene has 245 entities from tick 0 on and never creates or destroys any.

## The look

Flat pixel art in a 16-pixel grid at scale 2: bevelled blocks on a dark navy field, a thin grey frame, an amber accent,
grey labels and white values, from the engine's own art only (`SpriteBevel`, the white square, the Blocks sheet and the
default font). Every colour is linear (`Color.New`; `Color.FromHex` is not used, since it reads its bytes as linear).
The bevel's face texel (sRGB 204, linear 0.6038) multiplies a tint, so the bevel colours below are the target face
colour over 0.6038. `Assets/Scripts/Palette.luau` holds them.

| Name | Used on | Linear (r, g, b) | Shows as sRGB face |
|---|---|---|---|
| I, O, T, S, Z, J, L | well and preview cells (bevel) | (0, 0.7559, 1), (1, 0.7559, 0), (0.5051, 0.0748, 1), (0.0748, 0.7559, 0.0748), (1, 0.0528, 0.0528), (0.0351, 0.1329, 1), (1, 0.2582, 0.0116) | (0, 180, 204), (204, 180, 0), (150, 60, 204), (60, 180, 60), (204, 50, 50), (40, 80, 204), (204, 110, 20) |
| Other | blocks the tests set up (bevel) | (0.3111, 0.3111, 0.3697) | (120, 120, 130) |
| Ghost | the landing position (bevel) | (0.1014, 0.1262, 0.211) | (70, 78, 100) |
| Empty | the well's grid (bevel) | (0.0192, 0.0239, 0.0417) | (28, 32, 44) |
| Panel | HUD boxes, game-over panel (bevel, sliced) | (0.0351, 0.0452, 0.0849) | (40, 46, 64) |
| Button | RESTART (bevel, sliced) | (0.9565, 0.5051, 0.0351) | (200, 150, 40) |
| Background | clear, letterbox, the button's label | (0.0037, 0.0044, 0.008) | (12, 14, 22) |
| Frame, Label | bars, labels, help | (0.305, 0.3325, 0.4342) | (150, 156, 176) |
| Accent | top line, title, banner, GAME OVER | (0.8714, 0.4564, 0.0212) | (240, 180, 40) |
| White | values | (1, 1, 1) | (255, 255, 255) |

Text sizes are 9 to 24 canvas units (labels 10, values 14, banner 12, GAME OVER 16, help 9, title 24). The default
font's line box is about 1.21 times the size tall, and every text's rectangle holds it: `ui.tree {format: "Json"}`
reports `textOverflows` false for all 14 `UiText`s (the game-over panel's three only while it shows, at game over). That
is why the size-10 labels sit in 13-pixel rectangles, the size-14 values in 17 and the size-16 "GAME OVER" in 20
(session S2; they were 12, 16 and 18). The texts are top-aligned, so the taller rectangles moved no glyph. Every
rectangle, colour and text is serialized component data, so the state hash covers the look: any change to it means
re-recording the three replays.

## Scripts (`Assets/Scripts/`)

- `Pieces.luau`, `Board.luau`, `Bag.luau`, `Match.luau` (Modules, M17's board logic reused unchanged): shapes and SRS
  kicks; the 10 x 22 grid (`Fits`, `Place` reporting a top-out, `ClearLines`, `SetRow` for tests, `ToAscii`); the 7-bag
  over a `RandomGenerator`; one game as pure tick-driven rules (`Match.New(generator)`, `Step(controls)`, `Move`,
  `Rotate`, `HardDrop`, `HoldPiece`, `SpawnPiece`, `DropDistance`, `GravityTicks`, `Reset`, and `Events`, what the last
  step did).
- `Palette.luau` (Module): the linear colours above and `Palette.ForCell(name)` ("" empty, "Ghost", a piece, other).
- `Game.luau` (Behaviour on `/Game`): seeds the match with `Random.New(Random.Integer(0, 2147483647))` in `OnStart` (the
  session's only `Random` draw, which keeps M17's piece order), finds the HUD texts, the panel and the 16 preview cells
  by name, instantiates the 200 cells, then in `OnFixedUpdate` reads the actions, steps the match and mirrors the board,
  ghost, falling piece, previews, HUD values, banner and panel into components through shadow tables (only changes are
  written), and plays the sounds from the match's events. `OnUiClick` sets `RestartClicked` for `RestartButton`;
  `OnFixedUpdate` restarts once when the `Restart` action was pressed or the flag is set, then clears it (UI events run
  before `OnFixedUpdate` in the same tick), and counts restarts in `Restarts` (script state, not hashed).

Reading the running game (in a lockstep or paused session, `script_eval {context: "Play", entity: "/Game"}`):

```luau skip="notes"
local m = self.Match
return `{m.Piece} hold={m.Hold} next={table.concat(m.Queue, ",")} score={m.Score}\n{m.Board:ToAscii()}`
```

## Tests

- `Assets/Tests/Board.test.luau` (suite "Board", empty scene, Editor, Release and Dist, 19 cases, M17's reused
  unchanged): kicks at both walls and the floor, a blocked rotation, clears of 1 to 4 lines and their scores, levels,
  the 7-bag, the three game-over rules (each built so only its own rule can end the game), hold, gravity, soft drop, lock
  delay and its reset, DAS/ARR, hard drop at the ghost position, reset.
- `Assets/Tests/Tetris.test.luau` (suite "Tetris", `Main.scene`, `Isolation: "Case"`, Editor, Release and Dist, 14
  cases): the pool and the HUD at start (200 cells, each an active `SpriteRenderer` with no `MeshRenderer`, only the
  piece and its ghost off the empty colour, 16 preview `UiImage`s, the 14 HUD texts each a `UiText` with no `Text`
  component, values "0", "1", "0", the panel hidden, the camera pixel-perfect at 360 x 450 and 16 pixels per unit), a
  hard drop, taps and DAS to the wall, both rotations, hold and the hold slot's colour, "the next boxes show the queued
  pieces centred in their boxes" (after the known start, the O falling with J, S and Z queued, and again after a hard
  drop with S, Z and I queued: each NEXT box's four cells are active children of `NextBox<n>`, in the queued piece's
  colour, on that piece's literal cell positions in the box, `UiRect.Position` and the `Ui.GetRect` layout relative to
  the box agreeing, each 16 x 16), soft drop, the ghost (exactly 4
  ghost-coloured cells under the piece, before and after a move), a clear on `ScoreText` and `ClearText`, a tetris, a
  level rise, top-out then the `Restart` action, "game over shows the panel and its RESTART button restarts by a pointer
  click" and "a Restart action and a click in one tick restart once". The cases read `UiText.Text` and
  `SpriteRenderer.Color` through the component proxies; the pointer goes in view coordinates, twice the scene pixels
  `Ui.GetRect` reports (`ViewScale` 2: 720 x 900 over 360 x 450, no bars).
- Recording suites (`Modes: ["Editor"]`, `Main.scene`, `Isolation: "Suite"`): `Assets/Tests/Record/Opening.test.luau`
  ("Record.Opening", M17's planned taps, reused), `Assets/Tests/Record/LineClear.test.luau` ("Record.LineClear", M17's
  autopilot from `Assets/Tests/Lib/Autopilot.luau`, reused) and `Assets/Tests/Record/Restart.test.luau`
  ("Record.Restart": `HardDrop` taps every 3 ticks from tick 10 until game over, then the button's centre from
  `Ui.GetRect` doubled into view coordinates, `Test.InjectMouseMove` and `Test.InjectMouse("Left", "Tap", position)`,
  30 ticks, and expectations of the new game).

## Replays (`Assets/Tests/Replays/`)

| Replay | Recorded with | Ticks | Events | Checkpoints | FinalStateHash | Without input |
|---|---|---|---|---|---|---|
| `Opening.replay` | `test_run {filter: "Record.Opening", record: ...}` | 301 | 44 | 5 | `00e57904fd23c567` | fails at checkpoint tick 60 |
| `LineClear.replay` | `test_run {filter: "Record.LineClear", record: ...}` | 95 | 34 | 1 | `57a04e6df7415162` | fails at checkpoint tick 60 |
| `Restart.replay` | `test_run {filter: "Record.Restart", record: ...}` | 80 | 27 | 1 | `c661a4e2e2d7981a` | fails at checkpoint tick 60 |

Opening ends with O, J, Z, S and T locked, the I in the hold and the L falling over its ghost; LineClear with one line
cleared, score 100 and "SINGLE\n+100" showing; Restart with the new game after the click (score 0, panel hidden, the
first piece falling). Session S2 re-recorded all three from their suites after the HUD's text rectangles grew (S1's
hashes were `0213c7a651593710`, `96783a85ffdf5b86` and `c0f515fbf2d033b5`); each was verified twice in S2 with
`input_replay {verify: true, strictHash: true}` (equal hashes) and fails with `withoutInput: true`. The testing Dist
export's `--feature-test` reported the same three hashes. Any change to a hashed value (a colour, a rectangle, a text,
the camera, the seed, an input action, or a script that changes what is written) means re-recording all three through
their suites, verifying each twice and running the input check again.

## Status (end of session S3)

Session S1 built the whole game as above. Session S2, after the engine fixes of S1's friction (small text keeps its
strokes; `ui.tree` reports `textBounds` and `textOverflows`):

- L4: the eight size-10 and size-14 HUD texts and "GAME OVER" overflowed their rectangles by 0.1 to 1.4 pixels (line
  box 12.1 in 12, 16.94 in 16, 19.36 in 18); their `UiRect.Size` heights became 13, 17 and 20. No text, size, colour or
  position changed, and every `UiText` now reports `textOverflows` false.
- The three replays re-recorded and verified (above); `test_run {}` 38/38 green (19 Board, 13 Tetris, 3 recording,
  3 replays).
- `project_validate {}` no diagnostics; `script_check {}` no diagnostics (12 scripts); `project_upgrade {dryRun: true}`
  nothing to rewrite (43 files unchanged); `StartScene` and `Export.BuildScenes` both `Assets/Scenes/Main.scene`.
- Dist export with smoke test (ran, exit 0); testing Dist export with `featureTest` 35/35 and the three replay hashes
  equal to the editor's.
- The 10,000-tick check from the saved scene without input: no script error and no warning in the log, 245 entities, 0
  bodies and 0 voices at ticks 5,100 and 9,600 (script heap 1.46 MB then 1.41 MB); final hash `b5905527b5d7bc67`.
- Look screenshots kept in `Library/Automation/Kept/` (Play, Annotated, Clear, GameOver, Editor).

Session S3, for the lead's review of the staged rebuild, changed only the `Tetris` suite and this file:

- The new case "the next boxes show the queued pieces centred in their boxes" proves the three-piece next preview
  (Tests above). The pool case now also checks that every well cell is active and has no `MeshRenderer`, and that the
  14 HUD texts are `UiText` with no `Text` component. `.luaurc` is listed among the engine-consumed files.
- A test suite is not in the scene, so no hashed value changed: each replay verified once with `input_replay {verify:
  true, strictHash: true}` at its committed hash (Opening `00e57904fd23c567`, 5 checkpoints; LineClear
  `57a04e6df7415162`, 1; Restart `c661a4e2e2d7981a`, 1).
- `test_run {}` 39/39 green (19 Board, 14 Tetris, 3 recording, 3 replays); `script_check {}` no diagnostics (12
  scripts); `project_validate {}` no diagnostics; `project_upgrade {dryRun: true}` nothing to rewrite (43 files
  unchanged); no warning or error in the log.
- The editor ran with `renderer: "none"` (no screenshot or export was needed; a rendering launch waited on the GPU
  lock), so the testing Dist export's `featureTest`, now 36 cases, was not rerun.

Not verifiable from automation: the games suite (Debug and Release editors and the testing Dist export's hashes) and
`Scripts/AuditProvenance.py`, and the soak's game-view rendering; the lead runs them from the repository.
