# 0036 — M25 Tetris rebuilt in place as pixel art

- **Status:** accepted; M25's build plan. M25 has no contract (ADR 0028 decision 8, the plan's §3.6 "M25"): it freezes no interface, and this record is what the builder sessions, the lead's triage, the friction fixes and the landing follow. It is committed on `milestone/m25` as a documentation-only checkpoint (ADR 0028 decision 11). The friction list, the session record, the "Hit as" column of the predicted friction and "Before milestone completion" are filled in as the milestone runs. Lead-owned.
- **Amended 2026-11-11 (the lead's decisions before session 1):** the builder's reads, `Restart.replay`, the skills' Tetris text, Inter at scale 2, the builder window, and builder sessions as background agents of the lead's session ("Lead's decisions before session 1"). Context, decisions 5, 6, 7 and 11, the verification plan, risks 2 and 3, P01, P02 and P07, and Roadmap M25's "Parallelization" changed with them.
- **Amended 2026-11-13 (the landing and the milestone review):** the merge of main and its redo, the milestone review's findings and fixes, builder session S4 (the lead's decision, for the one finding on the game's test suite) and its friction F15 ("Landing", "Milestone review", the session record and "Before milestone completion"). Decisions 6 and 8 and the lead's decision b changed with them.
- **Amended 2026-11-13 (the soak):** the acceptance soak's three failures, their two causes and the fix of the first-party one, the job workers' profiler rings, and, for the driver's, A-prime, the lead's refinement of the product owner's option A: the soak's memory rule measures the C runtime's heap, its growth bounded by 1 % of the private bytes ("Landing", "The soak").
- **Date:** 2026-11-11; the lead's decisions before session 1 the same day; the landing and the milestone review on 2026-11-13.
- **Base:** `c36b0315` (phase 3's reservation ledger, the head of `setup/phase3`, which main fast-forwards to once the lead's PreCommit is green), branch `milestone/m25`.
- **Authority:** Roadmap "Rules for every milestone", M17 and M25; ADR 0028 decisions 8, 10, 11, 12, 14, 15 and 17 and its "Phase 3 reservation ledger"; ADR 0023; ADR 0026 (the builder's rules: decisions 3, 4, 9, 15, 16 and 18, frictions F27 and F30, predicted P10); ADR 0027 (decisions 6 to 11, frictions F03 and F08, the session record); ADR 0029 (M18's output stage and SpriteSheet); ADR 0030 decision 12 (distinct stems and Names); ADR 0032 (M21: sprites, sorting, the PixelPerfectCamera, the built-in sprite textures, tilemaps); ADR 0033 (M22: the UI framework, its input and its state-hash block). The lead's plan (`bin/Reviews/Backlog-Plan.md`, revision 5: §3.6 "M25" and "The Tetris exception", §4.3's phase-3 row, §4.4, §4.5's bridge row, §6 "M25" and the Appendix row TETRIS-2D) is kept under the ignored `bin/Reviews/`.

## Context

- **M25 in phase 3.** M25 is developed beside M24 (visual scripting and editor windows, which lands at any time) and M26 (effects, post-processing and terrain, which lands last). It lands before M26 with a plain gate, `CI.py --require-clang-cl`. Its acceptance's soak then runs as a targeted `Hardening.py --steps soak` on the gated tree, before main fast-forwards (ADR 0028 decisions 12 and 17). The work is about 4 sessions plus 2 to 3 builder sessions: this plan, a documentation checkpoint, 1 or 2 friction-fix streams by the module owners, and the landing.
- **Tetris today** (`Projects/Tetris` at `c36b0315`). M17's build is 3D:
  - 78 tracked files: 13 materials, `Cell.prefab` (a `MeshRenderer` cube), `Main.scene`, five scripts, five test scripts, nine `.sfx` sounds, two replays, the `.eproj`, `.luaurc`, `.gitignore`, `AGENTS.md`, `Automation/BuildLog.jsonl` and `Automation/Provenance.json`.
  - The scene holds a camera, a sun, an Environment with a PostProcess, stage meshes and a Screen `Text` HUD; `OnStart` pools 200 well cells and 16 preview cells from the prefab.
  - Replays: `Opening.replay` (301 ticks, 44 events, 5 checkpoints) and `LineClear.replay` (95 ticks, 34 events, 1 checkpoint), last re-recorded by M20 (`5b96177faa088fd0`, `18c6b54ec7f80aff`) and upgraded since by `Editor --upgrade` through M23.
  - The transcript has 578 lines: three bridge connections (sessions S1 and S2, M20's re-record) and six `cli` upgrade pairs. Provenance has 74 entries.
- **What the engine offers now** (the game-building skill's `sprites-2d` and `ui` pages; ADR 0032 and ADR 0033).
  - **Sprites.** `SpriteRenderer {Sheet, Frame, Texture, Color, FlipX, FlipY, PixelsPerUnit, SortingLayer, OrderInLayer, Lit, NormalMap}`. `Color` is linear and multiplies every texel. There is no visibility field: an alpha-0 sprite is still picked and bounded.
  - **Built-in art.** Four procedural 16 × 16 RGBA8 sRGB shapes, never compressed: `engine://Textures/SpriteSquare`, `SpriteRounded`, `SpriteBevel` and `SpriteCircle`.
    - All are white except the bevel. Its 2-texel lit top and left edges are 255, its 2-texel shaded bottom and right edges 128 and its face 204, in sRGB (`Engine/Source/Engine/Asset/BuiltinTextures.h`).
  - **PixelPerfectCamera** `{ReferenceWidth, ReferenceHeight, PixelsPerUnit, LetterboxColor}`.
    - It renders at the reference resolution through the 2D colour path: a linear tonemap at exposure 0, with no bloom, dither or FXAA, whatever a PostProcess says.
    - It upscales by the largest whole factor that fits the view, with point sampling and letterbox bars, and snaps the camera to the pixel grid.
  - **Tilemaps.** One `Color` per renderer, no per-tile tint, no script writes to cells. `tilemap.paint` edits are deferred while a lockstep, recording, replay or test session runs.
  - **UI** (`UiCanvas`, `UiRect`, `UiImage`, `UiText`, the four widgets).
    - It is laid out, hit-tested and drawn in scene pixels, which under a PixelPerfectCamera are the reference resolution, after every sprite and before the upscale.
    - `UiImage` draws a sprite-sheet frame (plain with a null sheet, or 9-sliced).
    - `UiText` draws the default font, Inter, from a signed-distance atlas with soft edges. `Wrap` is on by default.
    - Clicks reach behaviours through `OnUiClick`, which bubbles from the element to its ancestors.
    - UI input consumes no key: the game's actions and UI navigation both see every press.
- **What automation offers.**
  - `Tools/MCP/catalog.json` lists 69 proxied tools, and the bridge adds seven of its own (`engine_call` among them).
  - The lead's current Claude Code session, and every subagent of it, lists 65 of those 69. It lacks `ui_tree`, `ui_create_widget`, `tilemap_paint` and `animation_preview`. That is the stale tool list of ADR 0026 F27. A builder session, a background agent of the lead's session (decision 6), reaches those four methods through `engine_call`: `ui.tree`, `ui.createWidget`, `tilemap.paint` and `animation.preview` (P01).
  - These are reached through `engine_call` only: `viewport.setOptions` (`mode2D`, `gameResolution`, `uiPreview`), `viewport.camera`, `viewport.frame`, `viewport.pick`, `stats.get`, `play.state`, `test.list`, `project.refreshAssets` and `asset.setImportSettings`.
- **Every deterministic session runs at the project's window size:** lockstep play, `test.run`, recording, replay and the Dist `--feature-test`. Tetris's window is 720 × 900 and not resizable, so the output scale and the pointer mapping are the same in every run of every configuration.
- **The audit and the games suite read the index.**
  - `AuditProvenance.py` and the games suite materialize the staged blobs. They also need the project directory to exist on disk; a deleted one is `project-missing`, exit code 3 (`Scripts/Lib/provenance.py`).
  - `project.create` accepts only a missing or empty directory. Any entry is refused with `AlreadyExists`: `Library/`, a `*.bak` file or `AGENTS.md`.
  - The soak copies the working tree, not the index, and skips a project without an `.eproj`.
- **The ledger binds M25** (ADR 0028, phase 3):
  - `Projects/Tetris` is M25's alone. A sibling that lands before M25 upgrades Breakout and NativeTest only.
  - `D:\Engine` holds M25's branch for the builder window, then M26's re-record window follows.
  - M25 adds no component, format, AssetType, built-in asset, settings section, phase, hash block or golden. A friction fix that needs FeatureTest coverage takes IdSeeds 2300–2399.
  - A friction fix in a file the ledger gives no owner (`Exporter.cpp`, the pak, `games.py`, `provenance.py`, `Lint.py`, `SceneSerializer`, the method-support files, the font and text pipeline, ...) needs a ledger amendment on main first.
  - M26 re-records the rebuilt Tetris only if its scenes hold a PostProcess.

## Decisions

### 1. M17's feature list, and its 2D and UI form

The acceptance is M17's feature list (Roadmap M17 "Features"; `Projects/Tetris/AGENTS.md` at `c36b0315`) on sprites and UI. No feature is added or dropped. M17's notes list T-spins, drop points, a line-clear flash and pause as left out, and they stay out. That way the reused rules and recording plans (decision 5) stay valid, and each row keeps a test.

| M17 feature, as built | M25 form | Proved by |
|---|---|---|
| A 10 × 20 visible well of 22 rows; pieces spawn in rows 21 and 22 and drop one row at once; rows 21 and 22 are never drawn | `Board.luau` unchanged. 200 sprite cells (rows 1–20) under the PixelPerfectCamera (decision 2), always active, each showing the empty, ghost or piece colour | `Board.test`; Tetris "pool" case |
| 7 tetrominoes with SRS rotation and the JLSTZ and I wall-kick tables | `Pieces.luau` unchanged | `Board.test` kicks at both walls and the floor |
| 7-bag randomizer over `Random.New(Random.Integer(0, 2147483647))`, made in `OnStart`; `Simulation.Seed` 1717 (first bag O, J, S, Z, I, T, L) | `Bag.luau` unchanged. The seed is kept, and so is the session's `Random` draw order (one `Random.Integer` in `OnStart`), so the first bag and the reused recording plans hold | `Board.test` 7-bag; the replays |
| Gravity by level: a frozen ticks-per-row table at 60 Hz (48, 43, 38, 33, 28, 23, 18, 13, 8, 6, 5, 5, 5, 4, 4, 4, 3, 3, 3, then 2 to level 29, then 1) | `Match.luau` unchanged | `Board.test` gravity |
| Soft drop (a row every 2 ticks), hard drop, lock delay (30 ticks, reset by a move or rotation of a grounded piece at most 15 times per row), DAS 10 and ARR 2 ticks | `Match.luau` unchanged | `Board.test`; Tetris cases |
| Ghost piece | The cells of the landing position show the ghost colour | Tetris "ghost" case |
| Hold, once per piece; a three-piece next preview | UI: a HOLD box and three NEXT boxes, each with 4 `UiImage` cells (16 preview cells, decision 3) | `Board.test` hold; Tetris "hold" case |
| Line clears scoring 100/300/500/800 × level; a level every 10 lines | `Match.luau` unchanged. The values are UI `UiText` labels | `Board.test`; Tetris "clear", "tetris" and "level" cases |
| Game over by block-out, lock-out or a lock with a block above the well | `Match.luau` unchanged. The game-over panel (UI) shows | `Board.test`'s three game-over cases; Tetris "top-out" case |
| Restart at any time by the `Restart` action | Unchanged. At game over the panel's RESTART button also restarts, by pointer, once per tick whichever way (decision 3) | Tetris "top-out" and "click" cases; `Restart.replay` |
| HUD: score, level and lines; the last clear ("SINGLE +100" … "TETRIS! +800", 120 ticks); "GAME OVER / Press Enter to restart"; the title, HOLD, NEXT and the controls help | UI only: `UiText` labels and values, the clear banner and the game-over panel in one Screen canvas (decision 3). No `Text` component anywhere | Tetris HUD cases; decision 9's component check |
| Nine synthesized one-shot `.sfx` sounds: move, rotate, lock, hard drop, clear, tetris, hold, level up, game over; none loops | The same nine assets with the same values, recreated through `asset_create` (decision 5) | the soak's voices; the builder's check |
| The look: PBR cube cells with HDR emission, PCSS shadows on a backboard, GTAO, bloom, ACES | Replaced by the pixel-art look of decisions 2 and 4: no mesh, light, Environment or PostProcess | the look review (decision 4); decision 9's component check |
| No physics bodies; a constant entity count from tick 0; texts and materials written only on change | Unchanged: no body or collider, a pool made once in `OnStart`, every component write only on a change | the soak; the builder's 10,000-tick check |

**Project settings,** set with `project_set_settings`:

| Section | Value |
|---|---|
| `Name`, `StartScene` | `Tetris`; `Assets/Scenes/Main.scene` |
| `Window` | Title `Tetris`, 720 × 900, `VSync` true, `Fullscreen` false, `Resizable` false |
| `Simulation` | `FixedHz` 60, `MaxStepsPerFrame` 5, `Seed` 1717, `MaxEntities` 65536 |
| `Input.Actions` | M17's eight Buttons, bindings unchanged: `MoveLeft` (Key.Left, Key.A, Gamepad.DPadLeft), `MoveRight` (Key.Right, Key.D, Gamepad.DPadRight), `SoftDrop` (Key.Down, Key.S, Gamepad.DPadDown), `HardDrop` (Key.Space, Key.W, Gamepad.DPadUp), `RotateCW` (Key.Up, Key.X, Gamepad.South), `RotateCCW` (Key.Z, Key.LeftControl, Gamepad.East), `Hold` (Key.C, Key.LeftShift, Gamepad.LeftBumper), `Restart` (Key.Enter, Key.R, Gamepad.Start) |
| `Scripting` | `MemoryLimitMB` 256, `CallbackBudgetMs` 1000, `PauseOnError` true, `BlockPlayOnTypeErrors` true |
| `Export` | `BuildScenes` [`Assets/Scenes/Main.scene`], `Exclude` [`Assets/Tests/**`], `Version` `2.0.0`, `Company` `Engine Demos`, no icon, `PruneUnreferenced` false |
| `Testing` | The five suites of decision 8, `Replays` [`Assets/Tests/Replays/*.replay`], `CaseTimeoutTicks` 600, `SuiteTickLimit` 36000, `CoverageGates` false |
| `SortingLayers` | [`Default`, `Board`, `Frame`] (decision 2) |
| `Ui` | `Navigation` false; the other members keep their defaults (decision 3) |

The other sections keep the `Empty` template's defaults. Physics and Rendering are unused.

There is one scene, `Assets/Scenes/Main.scene`: M17 had no menu, and one scene keeps the soak at one Tetris subject (the ledger's estimate).

### 2. The board, the pieces and the camera: sprites under a pixel-perfect camera

- **Sprites, not a tilemap.** The well is a pool of 200 `SpriteRenderer` cells. A Tilemap fails all three things the well needs:
  - **A colour per cell.** A palette entry has no colour, and the renderer has one `Color`.
  - **Writes at run time.** Scripts cannot paint cells.
  - **Effect inside test and replay sessions.** `tilemap.paint` waits for them to end.
  
  It would also need a coloured tileset texture, and the builder cannot make a texture with pixel data. 200 sprites over one texture batch into one QuadBatcher draw (ADR 0032 decision 4). No Tilemap is used, for decoration either.
- **The cell.**
  - `Assets/Prefabs/Cell.prefab`: Transform plus `SpriteRenderer {Texture: "engine://Textures/SpriteBevel", Sheet: null, PixelsPerUnit: 16, SortingLayer: "Board", Color: <Empty>}`.
  - `OnStart` instantiates it 200 times under `/Game/Cells` with `Scene.Instantiate(prefab, vector.create(column - 5.5, row - 10.5, 0), nil, cells)`. That is M17's mapping, column 1–10 and row 1–20, row 1 at the bottom.
  - A cell is never hidden. Each tick the behaviour writes a cell's `Color` only when it changes, to the empty, ghost, piece or "other" colour of decision 4.
- **The camera.** `/Camera` holds:
  - `Transform {Translation: [0, 0, 10]}` with the identity rotation, looking down -Z;
  - `Camera {Projection: "Orthographic", Primary: true, Clear: "Color", ClearColor: <Background>, NearClip: 0.1, FarClip: 100}`;
  - `PixelPerfectCamera {ReferenceWidth: 360, ReferenceHeight: 450, PixelsPerUnit: 16, LetterboxColor: <Background>}`.
- **Why 360 × 450 at scale 2.**
  - **It is the only clean fit.** It is the only reference at a scale of 2 or more that divides the fixed 720 × 900 window exactly and still holds a well of 16-pixel cells (160 × 320 pixels) with a HUD column of 96 pixels on each side, so no letterbox is drawn in any deterministic session.
  - **Smaller references need smaller cells.** 240 × 300 (× 3) and 180 × 225 (× 4) would need cells of 8 to 12 pixels. The only art is 16-texel shapes, and minifying them to those sizes samples on texel boundaries.
  - **Text stays legible.** Scale 2 also keeps the UI text, which is drawn at the reference resolution, as legible as the default font allows (predicted P07).
  - **Units and pixels.** One unit is 16 scene pixels and 32 screen pixels. Scene pixel (x, y), y down, is world (x / 16 − 11.25, 14.0625 − y / 16).
- **The frame** (`/Frame`, `SortingLayer` `Frame`). Four `SpriteRenderer` bars on the built-in white `SpriteSquare` (a null texture), scaled. Every edge falls on the 1/16-unit grid:

  | Bar | Translation | Scale | Colour |
  |---|---|---|---|
  | `WallLeft` | (−5.125, −0.125, 0) | (0.25, 20.25, 1) | Frame |
  | `WallRight` | (5.125, −0.125, 0) | (0.25, 20.25, 1) | Frame |
  | `Floor` | (0, −10.125, 0) | (10.5, 0.25, 1) | Frame |
  | `TopLine` | (0, 10.0625, 0) | (10, 0.125, 1) | Accent |

  `TopLine` marks the top of the visible well, as M17's accent line did.
- **Sorting layers.** `Board` holds the cells and `Frame` the bars. No two sprites overlap, but the order is declared rather than left to canonical entity order. UI always draws over every sprite, so sorting never involves the HUD.
- **Nothing else draws.** There is no Light2D, because sprites stay unlit: an unlit sprite's texels reach the screen exactly, which the look review checks (decision 4). There is also no Environment, PostProcess, mesh or 3D light:
  - The 2D colour path ignores PostProcess.
  - A scene without PostProcess keeps M26's PostProcess v2 re-record off Tetris (the ledger).
  - A light or Environment would only be another hashed component.

### 3. The HUD as UI

- **One Screen canvas.**
  - `/Game/HUD` holds `UiCanvas {ScaleMode: "ConstantPixelSize", ScaleFactor: 1, PixelSnap: true, Font: null, DefaultFocus: null, RenderMode: "Screen"}`: one canvas unit is one reference pixel, and the font is the default font.
  - It sits below `/Game`, so a click on the RESTART button bubbles to the `Game` behaviour's `OnUiClick`.
  - No behaviour sits on a HUD entity, because `SetActive` on the panel would run its lifecycle.
- **The layout.**
  - Every rectangle is anchored at its parent's top-left corner: `AnchorMin = AnchorMax = [0, 0]`, `Pivot [0, 0]`, `Position` the top-left corner and `Size` its size, in scene pixels (x right, y down).
  - The well's cells occupy x 100–260 and y 65–385, and the frame 96–264 by 63–389.
  - Every `UiText` has `Wrap` false and `RaycastTarget` false. Two-line texts use an explicit line feed.
  
  | Element | Kind | Position | Size | Text and style |
  |---|---|---|---|---|
  | `Title` | `UiText` | (100, 16) | (160, 32) | "TETRIS", size 24, Center, Middle, Accent |
  | `HoldLabel` | `UiText` | (8, 65) | (80, 13) | "HOLD", size 10, Left, Label |
  | `HoldBox` | `UiImage` Sliced | (8, 79) | (80, 48) | Blocks sheet frame 0, Panel colour |
  | `ScoreLabel`, `ScoreText` | `UiText` | (8, 141), (8, 155) | (80, 13), (80, 17) | "SCORE" size 10, Label; value size 14, Right, White |
  | `LevelLabel`, `LevelText` | `UiText` | (8, 181), (8, 195) | (80, 13), (80, 17) | "LEVEL"; value as above |
  | `LinesLabel`, `LinesText` | `UiText` | (8, 221), (8, 235) | (80, 13), (80, 17) | "LINES"; value as above |
  | `ClearText` | `UiText` | (8, 275) | (80, 32) | e.g. "SINGLE\n+100", size 12, Center, Accent; "" when idle |
  | `NextLabel` | `UiText` | (272, 65) | (80, 13) | "NEXT", size 10, Label |
  | `NextBox1`, `NextBox2`, `NextBox3` | `UiImage` Sliced | (272, 79), (272, 131), (272, 183) | (80, 48) | Blocks sheet frame 0, Panel colour |
  | `HelpText` | `UiText` | (8, 397) | (344, 48) | Three centred lines, size 9, Label colour: "LEFT/RIGHT MOVE   UP/X ROTATE   Z ROTATE BACK", "DOWN SOFT DROP   SPACE HARD DROP", "C HOLD   ENTER RESTART" |
  | `GameOverPanel` | `UiImage` Sliced, inactive until game over | (108, 185) | (144, 80) | Blocks sheet frame 0, Panel colour; centred over the well |
  | `GameOverTitle` (in the panel) | `UiText` | (0, 8) | (144, 20) | "GAME OVER", size 16, Center, Accent |
  | `GameOverHint` (in the panel) | `UiText` | (0, 30) | (144, 12) | "PRESS ENTER", size 9, Center, Label |
  | `RestartButton` (in the panel) | `UiImage` Sliced + `UiButton {Interactable: true, Focusable: false}` | (32, 50) | (80, 20) | Blocks sheet frame 0, Button colour; the only raycast target in the scene |
  | `RestartLabel` (in the button) | `UiText`, anchors stretched (`AnchorMax [1, 1]`, `Size [0, 0]`) | (0, 0) | (0, 0) | "RESTART", size 10, Center, Middle, Background colour |
  | `Preview1_1` … `Preview4_4` (slot 1 in `HoldBox`, slots 2–4 in the next boxes) | `UiImage` Simple, `RaycastTarget` false | written by the behaviour | (16, 16) | Blocks sheet frame 0, the piece's colour |
  
  - **The previews.** The behaviour centres each preview's bounding box in its 80 × 48 box. The offsets are whole pixels for every piece: I (8, 16), O (24, 8), the 3-wide pieces (16, 8). It writes each preview cell's `UiRect.Position` and `UiImage.Color` only when the slot's piece changes. The hold slot's four cells are inactive while the hold is empty.
  - **The values** are right-aligned in fixed rectangles, because Inter's digits are proportional.
  - **Heights from the line box** (S2's F13). A text's line box is the default font's ascent to descent, about 1.21 × its `Size`, taller than the size itself: the size-10 labels' lines are 12.1 pixels tall, the size-14 values' 16.94 and the size-16 title's 19.36. The labels' rectangles are therefore 13 high, the values' 17 and `GameOverTitle`'s 20, which S2 measured with `ui.tree`'s `textBounds` and `textOverflows` (F02), the game-over panel's in a lockstep session driven to game over (F10 adds `includeInactive` for that).
  - **Writes on change only.** The values, the clear banner (shown for 120 ticks after a clear) and the panel's active flag are written only when they change.
- **The Blocks sheet.** `Assets/Sheets/Blocks.spritesheet` is made with `asset_create {type: "SpriteSheet"}` over `engine://Textures/SpriteBevel`. It has one frame `Block`: `Rect` (0, 0, 16, 16), `Pivot` (0.5, 0.5), `Borders` 2 on every side. A `UiImage` cannot take a texture without a sheet, and the 2-texel borders keep the bevel's edges crisp when a panel is sliced.
- **Input.**
  - `Ui.Navigation` is false, `DefaultFocus` null and the button not `Focusable`.
  - The game's actions use the arrows, Space, Enter and the gamepad's South and East, which are the UI's default Submit, Cancel and direction bindings. UI input consumes no key, so with navigation on, a focused button would be clicked by every hard drop, and the gamepad's RotateCCW would raise `OnUiCancel`.
  - The keyboard and gamepad restart through the `Restart` action. The pointer clicks the button without taking focus: an unfocusable button is still activated by a press.
  - `OnUiClick(element)` sets a flag when `element.Name == "RestartButton"`. `OnFixedUpdate` restarts once when the `Restart` action was pressed or the flag is set, then clears the flag. Both arrive in the same tick: UI events run before `OnFixedUpdate`.
- **The ui hash block stays empty in ordinary play.** The pointer rests at (0, 0), and only the button catches the pointer. The block holds state only while the pointer hovers over the button or presses it.

### 4. The look: art direction and the look review

- **The direction.** Flat pixel art in a 16-pixel grid at scale 2: bevelled blocks on a dark navy field, a thin grey frame, an amber accent, grey labels and white values.
  - Only the engine's own art is used: `SpriteBevel` and `SpriteSquare`, the Blocks sheet, and the default font. Nothing is imported or downloaded.
  - **Colours are linear.** `SpriteRenderer.Color`, `UiImage.Color`, `UiText.Color`, `Camera.ClearColor`, `LetterboxColor` and `Color.New` all take linear values. `Color.FromHex` reads its bytes as linear without decoding sRGB, so it is not used.
  - **The bevel darkens its face.** Its face texel (sRGB 204, linear 0.6038) multiplies the colour, so the table's bevel colours are the target face's linear value over 0.6038.
  - **The scripts take the table's values.** A `Palette` module (`Assets/Scripts/Palette.luau`) holds them as `Color.New` values.
  
  | Name | Used on | Linear `Color` (r, g, b) | Shows as sRGB: face; lit edge; shaded edge |
  |---|---|---|---|
  | I | bevel | 0.0, 0.7559, 1.0 | 0, 180, 204; 0, 225, 255; 0, 112, 128 |
  | O | bevel | 1.0, 0.7559, 0.0 | 204, 180, 0; 255, 225, 0; 128, 112, 0 |
  | T | bevel | 0.5051, 0.0748, 1.0 | 150, 60, 204; 188, 77, 255; 93, 34, 128 |
  | S | bevel | 0.0748, 0.7559, 0.0748 | 60, 180, 60; 77, 225, 77; 34, 112, 34 |
  | Z | bevel | 1.0, 0.0528, 0.0528 | 204, 50, 50; 255, 65, 65; 128, 28, 28 |
  | J | bevel | 0.0351, 0.1329, 1.0 | 40, 80, 204; 53, 102, 255; 21, 47, 128 |
  | L | bevel | 1.0, 0.2582, 0.0116 | 204, 110, 20; 255, 139, 28; 128, 67, 8 |
  | Other (blocks the tests set up) | bevel | 0.3111, 0.3111, 0.3697 | 120, 120, 130; 151, 151, 164; 73, 73, 80 |
  | Ghost | bevel | 0.1014, 0.1262, 0.2110 | 70, 78, 100; 90, 100, 127; 41, 46, 60 |
  | Empty (the well's grid) | bevel | 0.0192, 0.0239, 0.0417 | 28, 32, 44; 38, 43, 58; 13, 16, 24 |
  | Panel (the HUD boxes and the game-over panel) | bevel, sliced | 0.0351, 0.0452, 0.0849 | 40, 46, 64; 53, 60, 82; 21, 25, 37 |
  | Button | bevel, sliced | 0.9565, 0.5051, 0.0351 | 200, 150, 40; 250, 188, 53; 125, 93, 21 |
  | Background (clear and letterbox colour; the button's label) | flat | 0.0037, 0.0044, 0.0080 | 12, 14, 22 |
  | Frame and Label | flat (`SpriteSquare`, text) | 0.3050, 0.3325, 0.4342 | 150, 156, 176 |
  | Accent | flat | 0.8714, 0.4564, 0.0212 | 240, 180, 40 |
  | White (values) | text | 1.0, 1.0, 1.0 | 255, 255, 255 |

  Alpha is 1 everywhere. The canvas's default theme tints the button by state.
- **Cells and text sizes.**
  - A cell is 16 × 16 scene pixels (32 × 32 on screen). The previews use the same cell size.
  - Text sizes are 9 to 24 canvas units, never below 9, because the default font's signed-distance edges soften by about a pixel at the reference resolution.
- **Finish the look before recording.** Every UI rectangle, colour and text is serialized component data, so the state hash covers it (M17's F03). S1 records its replays only after the look holds, and S2 records them again after any look change.
- **The look screenshots.**
  - The builder keeps each with `keep: true`, so it lands in `Library/Automation/Kept/`, which is never pruned. It reports each path with its name.
  - After each session the lead copies them to `bin/Reviews/M25-Look/S<n>/<Name>.png` before the next editor launch on the project (ADR 0027 decision 9's copy rule). Nothing is committed.
  - Every game-view capture uses `view: "Game", target: "Play", width: 720, height: 900`. The default 640 × 360 crops a 360 × 450 reference at scale 1, and the default `maxDimension` of 1024 leaves 720 × 900 unscaled.
  
  | Name | Taken |
  |---|---|
  | `Play` | `viewport_screenshot` at the end of an `input_replay {path: "Assets/Tests/Replays/Opening.replay", verify: true, strictHash: true}` (five pieces locked, the I in the hold, the L falling over its ghost) |
  | `Clear` | `viewport_screenshot` at the end of `LineClear.replay`'s verification (one line cleared, "SINGLE +100", score 100) |
  | `GameOver` | `viewport_screenshot` in a lockstep session driven to game over with `HardDrop` taps through `play_step` (the panel and its button) |
  | `Annotated` | `Play`'s state again with `annotate: {labels: "all"}`, for the rectangles only |
  | `Editor` | `editor_screenshot {keep: true}` during that lockstep play, with the Game view at 720 × 900 (`engine_call viewport.setOptions {gameResolution}`) and the Scene view in 2D mode (`mode2D: true`). It is for the lead's view of authoring; the look is judged on the game-view captures |
- **The look review.** After each session the lead checks the kept `Play`, `Clear` and `GameOver` captures. Each failed check becomes a finding (L-01, …) in the friction list, which the next session fixes.
  - **L1 grid.** The image is the 360 × 450 scene doubled, with no bars. Every colour, glyphs included, comes in aligned 2 × 2 blocks, and every edge of a cell, bar, panel or preview cell falls on an even screen pixel.
  - **L2 palette.** A face pixel of each visible piece, the ghost and an empty cell is within 1 of its sRGB target in every channel, sampled with any colour picker or a short script. The lit and shaded edges show. The background is (12, 14, 22).
  - **L3 board.** There are 10 × 20 cells of 32 screen pixels. The well spans screen x 200–520 and y 130–770. The frame is 8 screen pixels wide, and the accent line sits at the top of the well.
  - **L4 HUD.** Every text lies inside its rectangle (the `ui_tree` rectangles and the image agree), with no wrap or clipping. Values are right-aligned. Nothing but the game-over panel covers the well. Every text is readable at 100 % zoom.
  - **L5 state.** The previews, the hold, the ghost, the banner and the panel match the state that `script_eval` reports at the capture's tick.
  - **L6 nothing 3D.** Decision 9's component check passes on the working tree.

### 5. In place: the Tetris exception, the emptied folder, reuse and backups

- **The 3D build stays in the index and in history** until the finished pixel-art build replaces it in one checkpoint commit (decision 9).
  - Until then, `D:\Engine`'s index and every slot keep the 3D files, so every gate, games-suite and audit run on any tree tests the committed 3D build.
  - The pixel-art build exists only in `D:\Engine`'s working tree and in the backups (ADR 0027 decision 11's rule; ADR 0028 decision 8).
- **Before session 1, the lead prepares `D:\Engine`.** Run in Git Bash, from the root of `D:\Engine` unless a command names a path:
  1. Commit this record and the documentation checkpoint (decision 11), with `git -C D:/Engine/.claude/worktrees/m19 status --short` clean apart from untracked build output. Then free the branch: `git -C D:/Engine/.claude/worktrees/m19 switch --detach`.
  2. Check that no editor or bridge holds the project and no gate runs: `python Scripts/Locks.py`. Check that `git -C D:/Engine status --short` lists no tracked change. Then switch: `git -C D:/Engine switch milestone/m25`.
  3. Build what the bridge, the export and the triage run, before emptying:
     - `python Scripts/Generate.py`
     - `python Scripts/Build.py --config Debug`
     - `python Scripts/Build.py --config Release`
     - `python Scripts/Build.py --config Dist`
     - `python Scripts/GenerateDocs.py --check --config Debug`
     
     The bridge's `catalog.json` then matches the Release editor that it spawns.
  4. Make the builder's read-only reference copy of the 3D build:
     - `mkdir -p bin/Reviews/M25-Reuse`
     - `git archive --format=tar HEAD Projects/Tetris | tar -x -C bin/Reviews/M25-Reuse`
     
     This gives `bin/Reviews/M25-Reuse/Projects/Tetris/...`.
  5. Empty the working-tree folder, keeping the folder itself and the index: `find Projects/Tetris -mindepth 1 -maxdepth 1 -exec rm -rf -- {} +`. This also removes the ignored `Library/`, `Build/` and `*.bak` files, which `project.create` refuses.
  6. Verify, each printing as stated:

     | Command | Prints |
     |---|---|
     | `ls -A Projects/Tetris` | nothing |
     | `git diff --cached --quiet && echo index-unchanged` | `index-unchanged` |
     | `git ls-files Projects/Tetris \| wc -l` | 78 |
     | `git status --short Projects/Tetris \| grep -c '^ D '` | 78 |
     | `git status --short Projects/Tetris \| grep -vc '^ D '` | 0 |
- **What must not happen during the window.** `D:\Engine` holds `milestone/m25` for the whole builder window, which lasts days. Until decision 9's commit, nobody runs these there:
  - `git add`, `git rm`, `git commit` or `git stash` touching `Projects/Tetris` through `D:\Engine`'s own index (decision 7's scratch index is the one exception);
  - `git checkout -- .`, `git restore`, `git reset --hard` or `git clean`.
  
  Friction fixes reach the branch by fast-forward (decision 7) and never touch `Projects/Tetris`. During the window, main moves only from slots, with `git fetch . <branch>:main` (plan §4.4; ADR 0028 decision 17 and phase 3's bridge-window row), because `D:\Engine` does not hold main. Builder sessions run by day, because the rendering editor holds the GPU lock (decision 6).
- **What the builder reuses, and how** (the plan's §6 "M25"). Reuse goes only through automation, so provenance and the transcript record it; nothing is copied as a file.
  - **Unchanged through `script_write`** (after `script_create` where a template applies), from `bin/Reviews/M25-Reuse/Projects/Tetris/`:
    - the board logic: `Assets/Scripts/Pieces.luau`, `Board.luau`, `Bag.luau` and `Match.luau`;
    - the pure-logic suite `Assets/Tests/Board.test.luau` (19 cases, empty scene);
    - `Assets/Tests/Lib/Autopilot.luau`;
    - the recording suites `Assets/Tests/Record/Opening.test.luau` and `Record/LineClear.test.luau`, which read only `GameScript.Game`'s `Match` member, which the new `Game` keeps.
  - **The same values through `asset_create {type: "SoundEffect"}`:** the nine `Assets/Audio/*.sfx`. They are already square-wave chiptune sounds.
  - **Read as a reference, then rewritten** for sprites and UI:
    - `Assets/Scripts/Game.luau`: its input reading, stepping and event-to-sound logic carry over. Its mirroring into materials, cube cells and `Text` does not.
    - `Assets/Tests/Tetris.test.luau`: its 11 cases are remapped (decision 8).
    - `AGENTS.md`: the old notes, which the builder replaces with its own.
  - **Never reused** (anything 3D): the 13 materials, the 3D `Cell.prefab`, `Main.scene` (camera, sun, Environment, PostProcess, stage meshes, the `Text` HUD) and the 3D look values.
- **Backups.** After each session, with the editor shut down, the lead copies the project without `Library/` and `Build/`, because the work has no history until decision 9:
  - `mkdir -p bin/Reviews/M25-Backup/S<n>`
  - `tar -c --exclude=./Library --exclude=./Build -C Projects/Tetris . | tar -x -C bin/Reviews/M25-Backup/S<n>`
  
  The lead also copies the kept screenshots (decision 4).
- **Main is merged only after the builder's last session,** and M25 keeps its own side of `Projects/Tetris` at that merge (decision 10).

### 6. The builder, its reads and the sessions

- **The builder.** One agent per session: a background agent of the lead's Claude Code session in `D:\Engine`, as M17's builder sessions were (ADR 0027 decision 6 and its session record), not a fresh top-level session. The lead starts it after the preparation (decision 5) or the between-session work (decision 7).
  - **The lead's tool list.** A background agent shares its parent's MCP connection and tool list (ADR 0026 F27), which lacks four of the catalogue's proxied tools (Context). The builder calls those four methods through `engine_call {method, params}`: `ui.tree`, `ui.createWidget`, `tilemap.paint` and `animation.preview` (P01). A method that a later catalogue change adds, or whose parameters it changes, is called the same way, so no session needs a fresh start for its tools.
  - **Check the tool list first.** The builder's first step loads the engine tools with ToolSearch and checks that `engine_call` and `docs_get` are among them. If either is missing, it stops and reports before any editor call.
  - **Tools.** It drives the editor only through the engine MCP tools that the lead's session lists (`mcp__engine__*`), with `engine_call` for the four methods above and for the methods without a tool (Context), and reads documentation through `docs_get`. The bridge transcribes every request, `engine_call`'s included, so the transcript and provenance rules below are unchanged.
  - **Reads and writes.** It may Read this record (its decisions 1 to 6 and 8 and "Predicted friction" are written for it) and the files below `bin/Reviews/M25-Reuse/` (M17's archived Tetris, for decision 5's `script_write` reuse), both read-only. It may Read and Write exactly one file, `Projects/Tetris/AGENTS.md`, and only after the project exists. It never reads engine source or any other file. These reads widen M17's one-file rule (ADR 0027 decision 6) for M25 only.
  - **The friction log.** It keeps one, as ADR 0027 decision 6 defines it: every awkward step, unclear error, missing method, documentation gap and workaround it was tempted to make, with evidence (the transcript line, the tool error). It returns the log with its report, and the lead stores it as `bin/Reviews/M25-Friction-S<n>.json`.
  - **One bridge user.** No other agent uses the bridge while a builder runs, the lead's own session included.
- **The prompt.** The lead writes it to `bin/Reviews/M25-Builder-S<n>.md` and starts the session's background agent with it. It quotes Roadmap M17's "Features" and Roadmap M25's "Acceptance" verbatim. It names this record's builder-facing decisions and the reuse folder. For S2 and S3 it also lists the triaged findings, each with its expected fix.
- **Transcript and provenance** (ADR 0026, ADR 0027; game-building's ground rules).
  - **A fresh transcript.** `editor_launch {project: "Projects/Tetris", create: true, template: "Empty", renderer: "vulkan"}` starts it. `Automation/BuildLog.jsonl` begins at line 1 with `project.create`, and `Automation/Provenance.json` begins anew. The 3D build's transcript stays in git history.
  - **Later sessions open it.** S2 and S3 launch without `create`, through a transcribed `project.open` and `session.info`.
  - **Every engine-consumed file is written by an automation method,** never by hand. The builder never edits or deletes the transcript or provenance.
  - **The rendering editor holds the machine-wide GPU lock** until it exits, so `editor_shutdown` ends every session, and sessions run by day.
- **The sessions.**

  | Session | Starts from | Goal | Time box |
  |---|---|---|---|
  | S1 | The emptied folder, `D:\Engine`'s Release build of `milestone/m25`, a fresh `editor_launch {create: true}` | The complete game, meeting decision 6's session checklist | about 300 tool calls (M17's 3D S1 took about 150) |
  | S2 | S1's project, after the triage, the look review and the friction fixes (decision 7); `editor_launch` without `create` | Every finding of the triage and the look review fixed; the replays re-recorded (decision 8) when a hashed value changed; the checklist again | about 150 tool calls |
  | S3 | As S2 | Only if S2's triage, or the milestone review's findings on the game (decision 10), leave work that only automation can do | about 150 tool calls |

  - **The lead may skip S2** only when S1's triage and look review leave nothing to change in the game, and no fix after S1 changed what its files or hashes depend on. The lead records that decision in the session record.
  - **A fourth session** needs the product owner's decision, which the lead asks for with the friction list.
  - **S4** ran on 2026-11-13 by the lead's decision at the milestone review, a deviation from this table's S3 reserve, which S3 spent: one finding on the game's own test suite, which only automation may change, with no hashed value changed, so the replays kept their hashes ("Landing", "Milestone review").
- **The session checklist** (the game-building skill's definition of done, through MCP):
  1. `project_validate {}` and `script_check {}` report nothing, lint warnings included.
  2. `project_upgrade {dryRun: true}` rewrites nothing.
  3. `test_run {}` is green: the five suites and the three replays (decision 8).
  4. Each replay comes from its recording suite. It is verified twice with `input_replay {verify: true, strictHash: true}`, with equal `stateHash`, and fails with `withoutInput: true`.
  5. `StartScene` is set, and `Export.BuildScenes` lists it.
  6. `project_export {config: "Dist", smokeTest: true}` gives `smokeTestRan: true` and `smokeTestExitCode: 0`.
  7. The 10,000-tick check runs from the saved scene, without a seed: `play_start {lockstep: true}`, `play_step` to tick 10,000, `script_errors {}` before any `script_eval`, and `engine_call {method: "stats.get"}` past ticks 5,000 and 9,500. Entities, bodies (0) and voices must not grow between the two reads.
  8. The look screenshots of decision 4 are kept and reported.
  9. `AGENTS.md` describes the scenes, actions, scripts, suites, replays (with their hashes) and the look. Each of its `luau` fences is code already run through `script_write` and `script_check`, or carries `skip="notes"`.
  10. The scene is saved and the editor shut down.
- **What ends a session:**
  - the checklist holds, with the report and the friction log returned;
  - the time box is reached, at a consistent point: scene saved, `AGENTS.md` status written;
  - a missing capability that automation cannot do. The builder records it as friction and stops, never working around it in the game (game-building's ground rules).

### 7. Between sessions: triage, friction fixes and the engine-change checklist

- **After each session the lead:**
  1. Makes the backup and copies the screenshots (decisions 4 and 5).
  2. Triages the friction log into this record's friction list. Each item resolves as a change with its regression test, or as a decision with its default in force. The lead fills in "Hit as" in the predicted friction.
  3. Runs the look review (decision 4).
  4. With no editor running, checks the rebuild on a scratch index, as M17's triage audited one. Every git call of the scripts inherits `GIT_INDEX_FILE`.
     - `mkdir -p bin/Reviews/M25-Scratch && cp .git/index bin/Reviews/M25-Scratch/index`
     - `GIT_INDEX_FILE=D:/Engine/bin/Reviews/M25-Scratch/index git add -A Projects/Tetris`
     - `GIT_INDEX_FILE=D:/Engine/bin/Reviews/M25-Scratch/index python Scripts/AuditProvenance.py --project Projects/Tetris`
     - `GIT_INDEX_FILE=D:/Engine/bin/Reviews/M25-Scratch/index python Scripts/Test.py --suite games --config Debug,Release,Dist --junit --require-gpu`
     - `python Scripts/Hardening.py --steps soak --soak-only "Tetris:*"`, a development aid that ends as a warning and soaks the working tree, its game view included.
     
     The games suite on that index is the first check of the Dist hashes, which the builder cannot run.
  5. Runs decision 9's component check on the working tree.
- **Friction fixes.**
  - Each fix is made in the engine by its module owner, never worked around in the game. It lands on `milestone/m25` as a checkpoint commit: one review pass, the targeted runs its change touches, and its regression test shown failing without the fix.
  - The fix agents work in the M25 slot or a stream slot, detached at `milestone/m25`'s tip, because `D:\Engine` holds the branch.
  - The lead brings each fix in with `git -C D:/Engine merge --ff-only <commit>`, which works beside the emptied folder because no fix touches `Projects/Tetris`.
  - Then the lead rebuilds `D:\Engine` (Debug, Release, Dist) and runs `GenerateDocs.py --check --config Debug`. A fix that changes a description, a method's parameters or the method set regenerates the references in its own commit. Because the builders share the lead's session and its bridge (decision 6), the lead restarts the bridge before the next session when a fix changed the bridge's code or `catalog.json`: it ends the bridge's `run.py` processes, and the client starts the bridge again on the next engine tool call (ADR 0027 decision 10).
- **The engine-change checklist** (ADR 0027 decision 10, for the lighter process):
  1. **Canonical output.** A fix that changes canonical output carries, in its own checkpoint, the recorded upgrade of the other audited projects: `Editor --headless --renderer none --audio-device none --user-data-dir <scratch> --project <P> --upgrade` from the slot's Release editor, for `Tests/Projects/Breakout` and `Tests/Projects/NativeTest`. If it changes FeatureTest, it also carries `Scaffold.py` and `Scaffold.py --check`. Tetris's own upgrade is `project_upgrade` in its next builder session, which the session record notes.
  2. **Simulation hashes.** A fix that changes simulation hashes is avoided. If the lead decides one is needed, it is committed only with everything it forces, as ADR 0027 decision 10 did:
     - The fix's slot commit stays off the branch. The lead applies it in `D:\Engine` with `git -C D:/Engine cherry-pick --no-commit <commit>` and rebuilds.
     - Breakout's `Opening` is re-recorded through the bridge in `D:\Engine` between builder sessions, by a driver with the builder's rules, as the only bridge user (ADR 0027 decision 10, step 3). NativeTest's replays are re-recorded the same way, and FeatureTest's through `Scaffold.py` and `Scaffold.py --check`.
     - The fix and the re-records are then committed together in `D:\Engine` as one reviewed checkpoint, staging only their own paths and never `Projects/Tetris`.
     - Tetris's replays are re-recorded in its next session.
  3. **No restaging.** No fix commit stages `Projects/Tetris`, and the fix's files follow the ledger's owners (decision 11).

### 8. Replays and testing suites

- **The suites** (`Testing.Suites`):

  | Suite | Script | Scene | Isolation | Modes | What |
  |---|---|---|---|---|---|
  | `Board` | `Assets/Tests/Board.test.luau` (reused) | none | Suite | Editor, Release, Dist | M17's 19 pure-logic cases: kicks, clears and scores, levels, the 7-bag, the three game-over rules, hold, gravity, soft drop, lock delay, DAS/ARR, hard drop, reset |
  | `Tetris` | `Assets/Tests/Tetris.test.luau` (rewritten) | `Main.scene` | Case | Editor, Release, Dist | M17's 11 integration cases on sprites and UI, driven by `Test.InjectAction`, plus three more (below) |
  | `Record.Opening` | `Assets/Tests/Record/Opening.test.luau` (reused) | `Main.scene` | Suite | Editor | Recording suite: M17's planned taps |
  | `Record.LineClear` | `Assets/Tests/Record/LineClear.test.luau` (reused) | `Main.scene` | Suite | Editor | Recording suite: M17's autopilot until a line clears |
  | `Record.Restart` | `Assets/Tests/Record/Restart.test.luau` (new) | `Main.scene` | Suite | Editor | Recording suite: a planned top-out, then a pointer click on RESTART |

  The `Tetris` suite's 11 cases:
  - **The pool and the HUD at start.** There are 200 cells with `SpriteRenderer`s in the empty colour, the 16 preview `UiImage`s and the HUD values "0", "1" and "0". The panel is inactive, and the camera is pixel-perfect at 360 × 450 with 16 pixels per unit. Since S3 (decision 9's review) it also checks that the 200 cells are active, with a `SpriteRenderer` and no `MeshRenderer`, and that the 14 HUD texts are `UiText` with no `Text` component; since S4 (the milestone review) that every well cell's `Transform` translation is exactly decision 2's (column − 5.5, row − 10.5, 0).
  - **A hard drop** locks the piece on the floor.
  - **Taps and DAS:** taps move the piece and DAS reaches the wall.
  - **Rotation:** both rotations turn the piece.
  - **Hold and its preview:** the hold stores the piece and the hold slot's cells show its colour; since S4, with the first piece the known O, the hold slot's four cells are active children of `HoldBox` at (24, 8), (40, 8), (24, 24) and (40, 24), 16 × 16, as `Ui.GetRect` lays them out (the NEXT boxes' check, generalised to take the box).
  - **Soft drop.**
  - **The ghost:** exactly 4 ghost-coloured cells in the landing row.
  - **A clear** shows on `ScoreText` and `ClearText`.
  - **A tetris.**
  - **A level rise.**
  - **Top-out, then the `Restart` action.**
  
  The cases beyond M17's, two planned and one from S3 (decision 9's review):
  - **"game over shows the panel and its RESTART button restarts by a pointer click."** The click goes in view coordinates.
  - **"a Restart action and a click in one tick restart once."**
  - **"the next boxes show the queued pieces centred in their boxes."** With the O falling and J, S and Z queued, each NEXT box's four cells are active, in `NextBox<n>`, in the queued piece's colour, on the piece's cell positions in the box, 16 × 16; again after one hard drop, with S, Z and I queued, which covers the I's position.
  
  The cases read `UiText.Text` and `SpriteRenderer.Color` through the component proxies.
- **The replays** (`Assets/Tests/Replays/`). Each is recorded from tick 0 by its recording suite with `test_run {filter: "<suite name>", record: "Assets/Tests/Replays/<Name>.replay"}`. No replay path contains a suite name.

  | Replay | Recording suite | Ends with | Proves |
  |---|---|---|---|
  | `Opening.replay` | `Record.Opening` | O, J, Z, S and T locked, the I in the hold, the L falling, the HUD as played | moves, both rotations, holds, a soft drop and hard drops on sprites and the UI previews |
  | `LineClear.replay` | `Record.LineClear` | one line cleared, score 100, "SINGLE\n+100" showing | a clear and the HUD's values and banner |
  | `Restart.replay` | `Record.Restart` | a game over, then the new game after a pointer click on RESTART: HUD reset, panel hidden, the first piece falling | the UI pointer path in every run mode, and the top-out and the shown panel only indirectly: its one checkpoint (tick 60) and its final hash (tick 80) both follow the click (ticks 50 and 51), which lands only on the shown button, and the new game's pieces follow the bag drawn before it; the `Tetris` suite's top-out cases check the game-over state itself (the lead's decision b) |

  - **What `Record.Restart` does.** It taps `HardDrop` from tick 10 until `Match.GameOver` holds (`Test.WaitUntil`). It reads the button's centre with `Ui.GetRect`, doubles it into view coordinates (a named constant: 720 × 900 over 360 × 450, no bars), then calls `Test.InjectMouseMove` and `Test.InjectMouse("Left", "Tap", position)`. It waits 30 ticks and asserts with `Test.Expect` that the game runs again with score 0 and an empty board below the new piece.
  - **Verification.** Each replay is verified twice with `input_replay {verify: true, strictHash: true}`, with equal `stateHash` values in the transcript. Each must fail with `withoutInput: true` added. M17's two failed at checkpoint tick 60: without input nothing locks before then, no line clears and no game ends.
  - **Re-recording.** Any change to a hashed value after recording means re-recording all three through their suites, verifying each twice and running the input check again. The hashed values include a colour, a rectangle, a text, the camera, a project setting the simulation reads (the seed, an input action), or a script that changes what is written.
- **The proof is the games suite's three halves** (`Test.py --suite games --config Debug,Release,Dist --require-gpu`, ADR 0027 decision 7):
  - each editor half (Debug and Release) runs the build-settings check, `script.check`, `project.validate`, the upgrade dry run, `test.run` with every replay at its committed hash and every checkpoint compared, and the input check of every replay;
  - the Dist half runs the testing Dist export's `--feature-test` with its hashes compared to both editors', then the plain Dist export's smoke test.
  
  The games suite does not replay twice in one session; three plays in three processes prove it.

### 9. The replacement commit

After the last builder session, with that session's triage, look review and scratch-index games suite green, the lead replaces the 3D build in one checkpoint commit in `D:\Engine`. Apart from decision 7's rare hash-changing fix, this is the only commit made there during the window, as in M19's and M20's bridge windows. With no editor running:
1. Stage the rebuild: `git add -A Projects/Tetris`.
2. Review `git status --short Projects/Tetris`: the 3D-only paths deleted, the rebuild's files added or modified, and no `Library/`, `Build/` or `*.bak`.
3. Run the component check on the staged files, which the working tree equals after step 1: no MeshRenderer cells, no Screen Text HUD, and decision 2's exclusions.

   ```
   python -c "import json, pathlib, sys; banned = {'MeshRenderer', 'SkinnedMesh', 'Text', 'DirectionalLight', 'PointLight', 'SpotLight', 'Environment', 'PostProcess', 'Light2D', 'RigidBody', 'RigidBody2D'}; found = sorted((str(p), c) for p in pathlib.Path('Projects/Tetris/Assets').rglob('*') if p.suffix in ('.scene', '.prefab') for c in json.loads(p.read_text(encoding='utf-8'))['ComponentVersions'] if c in banned or c.endswith('Collider') or c.endswith('Collider2D')); print(found or 'clean'); sys.exit(1 if found else 0)"
   ```
   
4. Run the audit and the games suite:
   - `python Scripts/AuditProvenance.py --project Projects/Tetris` (index mode);
   - `python Scripts/Test.py --suite games --config Debug,Release,Dist --junit --require-gpu`.
5. Take one review pass (the commit-review skill) over the game's diff: scripts, scene, prefab, settings, `AGENTS.md`, transcript and provenance.
6. Commit "M25: Tetris rebuilt in place as pixel art (sessions S1 to S<n>)". The trailer `Reviewed: approved; <n> findings (<n> fixed); checkpoint (games suite Debug, Release, Dist; AuditProvenance index; component check)` (ADR 0028 decision 11) is followed by the Co-Authored-By line.
7. Audit once from a fresh clone of `milestone/m25`, into an empty scratch directory outside the repository: `python Scripts/AuditProvenance.py --project Projects/Tetris` from the clone's root.
8. End the window:
   - remove the rebuild's ignored files with `git clean -fdX -- Projects/Tetris` (only `Library/`, `Build/` and `*.bak`);
   - `git switch main`;
   - re-attach the slot with `git -C D:/Engine/.claude/worktrees/m19 switch milestone/m25`.

### 10. The landing

In the M25 slot, after decision 9 (plan §4.4):
1. **Merge main** with `git merge main`, never a rebase. A conflict under `Projects/Tetris` takes M25's side (`git checkout --ours -- Projects/Tetris`, then `git add Projects/Tetris`), though no phase-3 sibling should touch it.
2. **Redo.**
   - **Registries.** M25's friction-fix entries are appended after main's (M24's) in the ledger's order: methods, bindings, validation codes in both tables of `ProjectValidator.cpp`, the shared lists.
   - **Generated files and builds.** Take either side, then run `python Scripts/Generate.py` and rebuild Debug, Release and Dist. Run `python Scripts/GenerateDocs.py --config Debug`.
   - **FeatureTest and fixtures.** `Scaffold.py` and `Scaffold.py --check`, only if a fix touched FeatureTest. The fixture generators' `--check`.
   - **Main's canonical upgrades on the rebuilt Tetris.** This is needed when the merge brought a recorded upgrade of Breakout or NativeTest, that is when their `BuildLog.jsonl` gained `cli` lines. Run `bin/Release-windows-x86_64/Editor/Editor.exe --headless --renderer none --audio-device none --user-data-dir <scratch> --project Projects/Tetris --upgrade`, then `python Scripts/AuditProvenance.py --project Projects/Tetris --files disk`.
3. **Run the targeted suites the merge touched.** These include the games suite when Tetris was upgraded or games tooling changed.
4. **Take one review pass** of the milestone's diff (`main...milestone/m25`: this record, the documentation checkpoint, the friction fixes, the rebuilt game, the merge's resolution) and fix every finding at once.
   - A finding on the game is fixed through automation in a builder session, the S3 reserve, with `D:\Engine` holding the branch again by decision 5's steps 1 and 2, and with no emptying.
   - Records: this record's friction list, session record and a landing section; Architecture §13.11's Tetris walkthrough rewritten for the pixel-art build (M25 owns it); the `Docs/Backlog.md` rows of any accepted limit or deferral.
5. **Gate.** `python Scripts/CI.py --require-clang-cl` on the final tree.
6. **Soak.** Then, on the gated tree, `python Scripts/Hardening.py --steps soak`: the full subject list without `--soak-only`, under the GPU lock it takes, about 45 to 50 minutes (the ledger). The gate's export stage leaves the current exported Release FeatureTest that its Runtime subject needs. A soak failure is fixed on `milestone/m25`, and the fixed tree is gated again (ADR 0023 decision 5).
7. **Fast-forward main** before M26 lands: `git -C D:/Engine merge --ff-only milestone/m25`, with `D:\Engine` on main and no bridge window open.

M26 then merges main and soaks and gates the rebuilt Tetris again. It has no Tetris re-record, because the rebuilt scenes hold no PostProcess (decision 2).

### 11. Ledger, ownership and records

- **Allocations** (phase 3's ledger):
  - this record is 0036;
  - IdSeeds 2300–2399, only if a fix needs FeatureTest coverage;
  - no AssetType, built-in, settings section, phase or hash block;
  - validation codes after M24's;
  - the skills appended in landing order.
  
  M25 regenerates no golden. An unexpected golden change is a reviewed regeneration named here.
- **Ownership of a friction fix.**
  - **A file M24 or M26 owns** (the post chain, the output stage, `ScreenshotMethods`, `ViewportMethods`, `QuadBatcher`, `ImGuiLayer`, `EditorApp`, `InspectorPanel`, the renderer's stage files, `PlaySession`, ...) is reconciled by the later lander at its merge. M26 lands after M25 and so reconciles every such fix. M24 does so only if it lands after M25.
  - **A file the ledger gives no owner** (`Exporter.cpp`, the pak, `games.py`, `provenance.py`, `Lint.py`, `SceneSerializer`, `EditorMethodContext`, `MethodSupport`, `BatchRunner`, `EntityMethods`, `PrefabMethods`, the font and text pipeline, `Vendor/stb`, ...) needs a ledger amendment on main first.
  - The friction list names each fix's owning module and its ledger row.
- **The documentation checkpoint before session 1.** It is made in the M25 slot, in the checkpoint of the lead's decisions before session 1 (that section, decision c), run with the `DocExamples` cases, the fits-inline case included, and `Lint.py`. The builder reads the skills through `docs_get`, so it reconciles the served skills with this plan. Tetris is described as being rebuilt in place as pixel art, and M17's 3D build as history (ADR 0027), not guidance:
  - **game-building `SKILL.md` "Definition of done".** The Tetris paragraph becomes M25's: the suites and three replays of decision 8, the look screenshots of decision 4, no MeshRenderer cells and no Screen Text.
  - **game-building `SKILL.md` §4.** The sentence on what a behaviour mirrors names sprite colours and UI text beside materials and `Text`.
  - **game-building `look.md` "Glow".** Its Tetris pair is attributed to M17's 3D build.

## Lead's decisions before session 1

Taken on 2026-11-11, before session 1. The text of each decision they touch was changed with them, as each item names.

- **a. The builder's reads: accepted.** Besides `Projects/Tetris/AGENTS.md`, which it reads and writes, the builder may read this record and the files below `bin/Reviews/M25-Reuse/` (M17's archived Tetris, for the `script_write` reuse of the board logic), both read-only. This widens M17's one-file rule (ADR 0027 decision 6) for M25 only. Changed: decision 6's "Reads and writes".
- **b. `Restart.replay` and its `Record.Restart` suite: accepted,** a third replay, verified in every run mode (both editors and the testing Dist export, through `Testing.Replays`), while its recording suite runs in the editor only (decision 8, unchanged). Its trailing restart is the play it records. That departs from the replays page's advice to end a recording with the played state in the scene, by design: a recording longer than one checkpoint interval that ends with a restart is accepted (ADR 0026 F29). Its one checkpoint (tick 60) and its final hash (tick 80) both come after the click on RESTART (ticks 50 and 51), so no compared hash holds the game-over state: they prove the top-out and the shown panel only indirectly (the click lands only on the shown button, and the new game's pieces follow the bag drawn before it), and the `Tetris` suite's top-out cases check the game-over state itself (worded so by the milestone review).
- **c. The skills' Tetris text: done** in this checkpoint, before session 1: game-building's "Definition of done" Tetris paragraph and its §4 sentence, and `look.md`'s "Glow". Changed: decision 11's documentation checkpoint.
- **d. Inter at scale 2: accepted for session 1.** If the look review after S1 finds the soft text unacceptable for pixel art, a hard-edged text option is engine friction. It is fixed in the engine on `milestone/m25`, after a ledger amendment on main gives M25 the font and text pipeline, which has no phase-3 owner today, and it is never worked around in the game. Changed: P07 and risk 2.
- **e. The builder window.** `D:\Engine` holds `milestone/m25` for the builder window, which lasts days. Meanwhile main moves only from a slot, with `git fetch . <branch>:main` (ADR 0028 decision 17 and phase 3's bridge-window row; plan §4.4). Builder sessions run by day, since the rendering editor holds the GPU lock. Changed: decision 5 and risk 3.
- **f. Builder sessions are background agents of the lead's session,** as M17's builder sessions were (ADR 0027), not fresh top-level sessions. Each drives the editor only through the engine MCP tools that the lead's session lists (loaded with ToolSearch), and through `engine_call` for the methods that list lacks: `ui.tree`, `ui.createWidget`, `tilemap.paint` and `animation.preview`, which is now P01's planned handling. The bridge's transcript and the provenance rules are unchanged. The lead writes each session's prompt to `bin/Reviews/M25-Builder-S<n>.md`. Changed: Context, decisions 6 and 7, P01, P02, and Roadmap M25's "Parallelization".

## Acceptance

Each line is followed by the run that proves it.

1. **M17's feature list on sprites and UI, as pixel art under a pixel-perfect camera.** Proved by decision 1's table, each row by its suite case: the `Board` and `Tetris` suites green in the games suite's three halves; and by the look review's L1 to L5 on the last session's screenshots.
2. **No MeshRenderer cells and no Screen Text HUD.** Proved by decision 9's component check on the staged files, and by the `Tetris` suite's pool case.
3. **Replays strict-hash identical in both editors and the testing Dist export, each failing without its input:** `Opening`, `LineClear` and `Restart`, each recorded from its suite and verified twice in the transcript. Proved by the games suite's three halves on the replacement commit, then by the gate's games stage.
4. **The suites green.** Proved by the same runs.
5. **`StartScene` and `Export.BuildScenes` set.** Proved by the games suite's build-settings check in each editor half.
6. **`project.validate` clean,** with `script.check` and the upgrade dry run clean too. Proved by each editor half.
7. **The Dist export's smoke test exits 0.** Proved by the games suite's Dist half, and by the builder's `project_export {config: "Dist", smokeTest: true}`.
8. **The provenance audit passes from a fresh clone.** Proved by `AuditProvenance.py --project Projects/Tetris`: in index mode on the committed tree (the games stage's audit step, which materializes the staged blobs as a fresh clone holds them), and once from an actual fresh clone (decision 9, step 7).
9. **The soak passes.** Proved by `Hardening.py --steps soak` on the gated tree (decision 10, step 6).
10. **The gate.** Proved by `CI.py --require-clang-cl` green on the final tree.
11. **Every friction item closed or decided,** every predicted item with its "Hit as", and the look review signed off by the lead. Proved by this record at the milestone review.

## Approvals

None needed. Nothing is downloaded or vendored, and no new kind of dependency or licence is involved. The art is the engine's built-in sprite shapes and default font (decisions 2 and 4). A pixel font would be a new asset download, which M21's default ("no CC0 packs") and this plan do not take.

## Risks

1. **Friction volume.** M16 recorded 34 friction items and M17 14. One or two fix streams are budgeted, with S3 in reserve. A fourth session goes to the product owner (decision 6).
2. **Text at the reference resolution.** The default font's signed-distance edges are point-upscaled; Inter at scale 2 is accepted for S1. If the look review rejects them, a hard-edged text mode is a change to the font and text pipeline, which the phase-3 ledger gives no owner: it needs a ledger amendment on main first, then lands on `milestone/m25` as a friction fix, never as a workaround in the game (P07).
3. **`D:\Engine` is pinned for days.** Main moves only from slots, by `git fetch . <branch>:main`. While a rendering editor holds the GPU lock, the siblings' GPU runs wait, so sessions run by day and end with `editor_shutdown`.
4. **The working tree differs from the index for the whole window.** An accidental stage, restore, stash or clean in `D:\Engine` would mix or lose builds. Decision 5's forbidden commands, the backups after each session and the single replacement commit bound it.
5. **A hash-changing engine fix during the window** forces re-records of Breakout, NativeTest and FeatureTest. It is avoided, and handled by decision 7's checklist when it cannot be.
6. **Reused recording plans assume M17's piece order.** That needs the kept seed and the one `Random` draw in `OnStart`. A changed draw order shows at once as a failing `Record.Opening` expectation.
7. **Determinism traps in the new presentation code:** `pairs` over entity keys, host queries, or reading layout rectangles to decide gameplay. The games suite's three plays catch a split, and the lead's scratch-index run after each session finds it early.
8. **A milestone-review finding on the game needs a bridge session** (S3), which delays the landing.
9. **Look subjectivity.** L1 to L3 are measured (grid, palette within 1, geometry); L4 and L5 are judged against `ui_tree` and `script_eval`.
10. **Later registrations rehash Tetris.** A new `SpriteRenderer` or UI field would. Phase 3 adds none (the ledger); a later one re-records Tetris's three replays through their recording suites, as any hash change does.

## Verification plan (ADR 0023)

- **This record:** a documentation-only checkpoint. `Docs/Decisions/` is not read by any check, so it runs only the `DocExamples` cases as a sanity run, plus one review pass against `Docs/ReviewChecklist.md`.
- **The documentation checkpoint:** the `DocExamples` cases (type-check, lint and fits-inline) and `Lint.py`, and one review pass.
- **Friction-fix checkpoints:** the fix's targeted build and test cases, its regression test shown failing without the fix (run alone), and once the suites the change touches. Those are the automation suite with a filter for a method change, and the games suite when the exporter, the games tooling or a demo changes. Agents never run `PreCommit.py` or `CI.py`.
- **After each session:** decision 7's triage runs: the scratch-index audit, the games suite, and the Tetris soak as a development aid.
- **The replacement commit:** decision 9's checks.
- **The landing:** the merge's targeted suites, one review pass, one `CI.py --require-clang-cl`, then the targeted soak. A red gate reruns only its failed stages unless the fix changed production code, build files or shared test support.

## Predicted friction

The lead fills "Hit as" from the friction list. Every handling fixes the engine or its documentation, never the game.

| Id | Predicted item (evidence) | Planned handling | Hit as |
|---|---|---|---|
| P01 | A stale tool list: the lead's session lists 65 of the catalogue's 69 proxied tools (`ui_tree`, `ui_create_widget`, `tilemap_paint` and `animation_preview` are missing) | Each builder session is a background agent of the lead's session (decision 6) and calls those four methods through `engine_call` (`ui.tree`, `ui.createWidget`, `tilemap.paint`, `animation.preview`), transcribed like every request. The bridge is not changed | S1: hit as planned, at no cost (the four methods through `engine_call`) |
| P02 | Methods reachable only through `engine_call`: `viewport.setOptions`, `viewport.camera`, `viewport.frame`, `viewport.pick`, `stats.get`, `play.state`, `test.list`, `project.refreshAssets`, `asset.setImportSettings` | Already documented (game-building, `sprites-2d`, `ui`). A method the builder reports as costly may become a tool through its registration flag: a catalogue change, after which the lead restarts the bridge, and the builders, who keep the lead's tool list, still call it through `engine_call` (decisions 6 and 7) | S1: hit as planned, at no cost |
| P03 | `project.create` refuses a non-empty directory (`Library/`, `*.bak`, `AGENTS.md`), and the bridge drops the transcript lines of a refused create | Decision 5's emptying removes ignored files. `AGENTS.md` is written only after the create | S1: not hit or avoided |
| P04 | No 2D project template (`Empty`, `Basic3D`) | `Empty` plus the first batches. A 2D template is not adopted | S1: not in S1's friction log |
| P05 | `viewport_screenshot` defaults to 640 × 360, which crops the 360 × 450 reference at scale 1 | Always 720 × 900 (decision 4), as the game-building skill says | S1: not in S1's friction log |
| P06 | Colours are linear; `Color.FromHex` reads bytes as linear; the bevel face scales a tint by 0.6038 | Decision 4's linear table and the `Palette` module. If sRGB input is still needed, an sRGB constructor is a script-API fix by Scripting's owner (add-script-api; `RegisterBindings` appended at the landing) | S1: not in S1's friction log |
| P07 | UI text at the reference resolution: Inter's signed-distance edges soften by about a pixel and are upscaled into 2 × 2 blocks; Latin-1 only; proportional digits; no pixel font | Sizes of at least 9, right-aligned values in fixed rectangles, ASCII labels. Inter at scale 2 is accepted for S1, and the look review judges legibility. If it finds the soft text unacceptable for pixel art, a hard-edged text option is engine friction, fixed on `milestone/m25` after a ledger amendment on main gives M25 the font and text pipeline (no phase-3 owner today), never worked around in the game. If the lead keeps the soft text, it is accepted with a Backlog §6 row | S1: hit as F01 (strokes under a pixel dropped at sizes 9 and 10), fixed in the engine |
| P08 | `UiText.Wrap` defaults to true; text is never clipped to its own rectangle; rectangles never follow their content | `Wrap` false and explicit line feeds (decision 3); `ui_tree` to check | S1: hit as F02 (`ui.tree` reported no text extent), fixed in the engine. S2: hit again as F13 (the default font's line box, about 1.21 × `Size`, overflowed nine of decision 3's rectangles), fixed in the documentation and decision 3's heights |
| P09 | `ui_create_widget`'s templates are sized for 1080p (a 200 × 50 button with a size-28 label, a 200 × 50 text at size 32) and focusable | Restyle with `entity_update` in the same batch, or `entity_create` with decision 3's components. A canvas-scaled template or a size parameter is an EditorCore Automation fix if the builder reports the cost | S1: not hit or avoided |
| P10 | UI navigation reads the game's keys (Space and Enter submit, the arrows navigate, the gamepad's East cancels), and no key is consumed | `Ui.Navigation` false, `DefaultFocus` null, the button not `Focusable` (decision 3; the `ui` page advises it) | S1: not hit or avoided |
| P11 | Pointer injection takes view coordinates, while `Ui.GetRect` and `ui_tree` report scene pixels; no API returns the output scale or bar offset | Twice the scene pixels, with no bars, at the fixed 720 × 900 window, as a named constant (decision 8). If hit, an output-mapping query in `Ui` or `Camera` is a script-API fix | S1: handled as planned, with a `ViewScale` constant of 2 |
| P12 | `RaycastTarget` defaults to on, and the pointer rests at (0, 0): a target over the top-left corner hashes UI state from tick 1 | `RaycastTarget` off everywhere but the button (decision 3) | S1: not hit or avoided |
| P13 | `OnUiClick` bubbles only through the clicked element's ancestors | The canvas sits below `/Game` (decision 3) | S1: not hit or avoided |
| P14 | A sprite has no visibility field, and an alpha-0 sprite is still picked and bounded | Well cells are never hidden; previews use `SetActive` (decisions 2 and 3) | S1: not hit or avoided |
| P15 | A Tilemap cannot tint per cell, take script writes, or change inside sessions | Sprites (decision 2) | S1: not hit or avoided |
| P16 | The only art is four 16 × 16 shapes: no pixel-data texture method, one texture per sheet, and a `UiImage` needs a sheet | The bevel, the square and the Blocks sheet (decisions 2 to 4). M21's default holds: a pixel-data texture method is an M25 fix only if the look review needs art the shapes cannot make | S1: not hit or avoided |
| P17 | The Scene view is never pixel-perfect, draws Screen canvases only with the UI preview and sprites only in the Lit view, and its 2D grid draws over sprites | The look is judged on game-view captures only; `mode2D` is for authoring (decision 4) | S1: not in S1's friction log |
| P18 | Annotated captures draw 13-pixel labels before the × 2 upscale | One annotated capture for the rectangles; `ui_tree` for the numbers | S1: hit as F04 (`labels: "all"` labels every sprite) |
| P19 | The served skills still describe the 3D Tetris (definition of done: AO and ShadowCascades screenshots; §4's "materials, HUD `Text`"; `look.md`'s Tetris pair) | The documentation checkpoint before S1 (decision 11) | S1: not hit or avoided |
| P20 | Look values are hashed, so a change after recording breaks all three replays (M17's F03) | Finish the look first; S2 re-records after look fixes; the divergence hint names the cause (decisions 4 and 8) | S1: not hit or avoided |
| P21 | The `Restart` action and a RESTART click arrive in the same tick | One restart per tick, with its own case (decisions 3 and 8) | S1: handled, with its passing case |
| P22 | `SetActive` on the game-over panel runs the lifecycle of behaviours on it | No behaviour on a HUD entity (decision 3) | S1: not hit or avoided |
| P23 | `Time.SetTimeScale(0)` stops the UI (Backlog) | Not reachable: M17's list has no pause | S1: not reachable |
| P24 | The builder cannot run the soak's game-view rendering: its 10,000-tick check renders through the Scene view (M17's F08) | The lead's Tetris soak after each session (decision 7) and the acceptance soak | S1: hit as F06, as planned (the lead's soak renders the game view) |
| P25 | Test helpers that read `Text` (M17's `textOf`) must read `UiText` through its proxy | The rewritten `Tetris` suite (decision 8); `luau-gameplay`'s UI chapter shows the proxy | S1: handled, with passing cases |
| P26 | Instantiating UI elements under a canvas needs a parent and `UiRect` writes | The 16 preview cells are authored in the scene under their boxes; only the 200 well sprites are instantiated (decisions 2 and 3) | S1: not hit or avoided |

## Friction list

Every awkward step, unclear error, missing method, documentation gap or tempted workaround a builder session reports, triaged by the lead, and every look-review finding (L-xx). Each resolves as a change with its regression test, or as a decision of this record with its default in force. The milestone review blocks on an open row.

| Id | Session | Symptom and evidence (BuildLog line, tool error, screenshot) | Predicted | Owning module (ledger owner) | Resolution |
|---|---|---|---|---|---|
| F01 | S1 | Major. Small UI text breaks up at the 360 × 450 reference: in the default font at sizes 9 and 10, strokes under a pixel wide (mostly vertical stems, and bars) drop out after the × 2 upscale ("HOLD" reads close to "IIOLD"; the I of "LINES", the E's of "SCORE" and "LEVEL", the P of "PRESS", the R of "RESTART", the K of "BACK"); sizes 12, 14, 16 and 24 read fine. Evidence: `bin/Reviews/M25-Look/S1/Play.png` and `GameOver.png` | P07 | Text rendering: `Passes/Text.slang` and `TextRenderer` (M25, by the ledger amendment of 2026-11-11: the rendering of text), and `Shared/TextVertex.h`'s `TextConstants`, the Text program's push constants, under the same row (recorded by ADR 0028's amendment "M25's milestone review", since the row names no shared header) | Fixed in the engine, commit `f9509d61`: "F01: small text keeps its strokes" below has the root cause, the fix and the goldens |
| F02 | S1 | Minor. `ui.tree` reports each element's rectangle but not the extent of its laid-out text, so automation cannot check L4's "every text lies inside its rectangle" | P08 | `ui.tree` (`UiMethods`; M25, by the ledger amendment of 2026-11-11) | Fixed in the engine, commit `7131d747`: each JSON entry reports `textBounds`, the box its text is laid out in (the lines' pen starts to their advance ends, the block's top to its bottom, where the Ui pass places it), and `textOverflows`, whether that box reaches beyond `rect` by more than 1/100 of a pixel; the text outline marks such an element `overflows` |
| F03 | S1 | Nit. Kept screenshots cannot be named: `keep: true` writes into `Library/Automation/Kept/` under the host's name | – | `ScreenshotMethods` (M26) | Not fixed in phase 3: the default stands, and the lead names the kept screenshots when copying them (decision 4). `Docs/Backlog.md` §6 row |
| F04 | S1 | Minor. An annotated capture's `labels: "all"` labels every sprite, so the 200 cells' labels bury the HUD's | P18 | `ScreenshotMethods`, `ViewportMethods` (M26) | Not fixed in phase 3: the lead judges the Annotated capture on the HUD labels. `Docs/Backlog.md` §6 row |
| F05 | S1 | Minor. No method reads a capture's pixel colours, so L2's palette check is not automatable | – | `ScreenshotMethods`, `ViewportMethods` (M26) | Not fixed in phase 3: the lead checks the palette with a colour picker on the copies. `Docs/Backlog.md` §6 row |
| F06 | S1 | Minor. The 10,000-tick check never renders the game view, and the Scene view runs the 3D chain (DepthPyramid, GTAO, Bloom, Tonemap, FXAA) for a sprite-only scene | P24 | The renderer's stage files (M26) | As planned: the lead's soak renders the game view (decision 7). The Scene view's 3D chain for a 2D-only scene (skipping passes with nothing to draw) is a `Docs/Backlog.md` §6 row, not fixed in M25 |
| F07 | S1 | Nit. `editor_screenshot` cannot bring the Game tab forward | – | `ScreenshotMethods` (M26) | Not fixed in phase 3: the Game view is taken from `viewport_screenshot`. `Docs/Backlog.md` §6 row |
| F08 | S1 | Nit. The `values` shape of `asset_create {type: "Prefab"}` is not documented where the builder looks | – | `asset.create`'s descriptions (`Editor/Source/EditorCore/Automation/AssetMethods.cpp`, whose descriptions no ledger row names; recorded by ADR 0028's amendment "M25's milestone review", which also tells M26, whose W26.1 rewrites the same description sentence for `Heightfield`, to merge M25's sentence at its landing and add `Heightfield` to the types that take no values), the game-building skill's `prefabs-and-levels` page | Fixed in the documentation, in the commit that records this triage: a Prefab takes no values (one empty root entity named after the file, as before), and `prefab_create` saves a built entity as a prefab; said in `asset.create`'s description, its `values` field and its `Prefab` type entry, and on the prefab page. Since the milestone review the refusal has a test ("AssetMethods: asset.create refuses values for a Scene, a Prefab and a Folder and writes nothing") |
| F09 | S1 | Nit. `docs_get` needs a connected editor | – | The MCP bridge (none) | Not fixed: launching the editor first costs nothing in the transcript. `Docs/Backlog.md` §6 row |
| F10 | S2 | Minor. `ui.tree` lists only active elements, so the game-over panel's texts, inactive until game over, could not be checked for L4 in the edit scene: it took a lockstep session driven to game over. Evidence: `bin/Reviews/M25-Friction-S2.json` | – | `ui.tree` (`UiMethods`; M25, by the ledger amendment of 2026-11-12) | Fixed in the engine, commit `2dd34dc4`: `ui.tree {includeInactive: true}` lays out a copy of the scene (the serializer's) in which every entity is active, so each inactive canvas and element is listed where it would land, its text measured, and marked inactive (JSON `active: false`, the outline's `inactive`); without it nothing changes, and the scene is only read |
| F11 | S2 | Nit. `ui.tree`'s JSON reports `scale` 0 for every element below the canvas (the canvas entry says 1). Evidence: `bin/Reviews/M25-Friction-S2.json` | – | `ui.tree` (`UiMethods`; M25, by the ledger amendment of 2026-11-12) | Fixed in the engine, commit `2dd34dc4`: every entry reports its canvas's scale, which turns its pixel measurements into the canvas units `UiRect` takes (the field's description had said 0 for an element) |
| F12 | S2 | Nit. `input_replay`'s failure data contradicted itself when a checkpoint stopped the replay: `checkpointFailed` true and a stop at checkpoint tick 60, yet `checkpointsFailed` 0 and `firstFailedCheckpointTick` 0, which only `continuePastCheckpoint` filled. Evidence: `bin/Reviews/M25-Friction-S2.json` | – | `Automation/Methods/ReplayMethods.{h,cpp}`'s failure data (M25, by the ledger amendment of 2026-11-12) | Fixed in the engine, commit `597cd1a5`: the checkpoint that stops a replay counts, `checkpointsFailed` 1 and `firstFailedCheckpointTick` its tick, as `continuePastCheckpoint` counts it, in `input.replay`'s result and error data and in `input.record` stop's verification data. It replaces ADR 0030's NT-09 decision ("by design"), and the game-debugging skill describes the new counts |
| F13 | S2 | Minor. The default font's line box is about 1.21 × `Size`, taller than decision 3's rectangles, and nothing in the `ui` page says how tall a UiText's line box is when sizing its rectangle: nine UiTexts overflowed (the labels 12.0996 in 12, the values 16.94 in 16, `GameOverTitle` 19.36 in 18), found with `ui.tree`'s `textOverflows`. Evidence: `bin/Reviews/M25-Friction-S2.json` | P08 | The game-building skill's `ui` page (documentation) | Fixed in the documentation, in the commit that records this triage: the `ui` page states a UiText's line box ((Ascent - Descent) × `Size`, about 1.21 × `Size` for the default font, which `ui_tree`'s `textBounds` measures) and advises sizing rectangles from `textBounds` and `textOverflows`, with `includeInactive` for hidden panels; decision 3's table takes S2's heights (the five labels 13, the three values 17, `GameOverTitle` 20), with which S2 fixed the game |
| F14 | S3 | Minor. `editor_launch {project: "Projects/Tetris", renderer: "vulkan"}` sent no response or progress for 1,800 s (2026-11-13 03:16:52 to 03:47:01 UTC), then Claude Code aborted the call (`MCP server "engine" tool "editor_launch" sent no response or progress for 1800s; aborting`), so the `LockUnavailable` that names the lock's holder never reached the builder. `editor_status` right after listed no editor, and a `renderer: "none"` launch then started at once, so the wait was for the machine-wide GPU lock that another worktree's GPU run held. Cause: the bridge's lock wait was bounded at 1,800 s (`LOCK_WAIT_SECONDS`) and reported the holder only on its own log (at the start, on a change and every 300 s), sending the client nothing, and Claude Code's idle timeout for a tool call is also 1,800 s, so the client gave up just as the bridge would have answered. Evidence: `bin/Reviews/M25-Friction-S3.json` | – (new) | The MCP bridge's tool-call progress and launch wait, `Tools/MCP/engine_mcp/server.py` and `launcher.py` (M25, by the phase-3 ledger as amended on 2026-11-13) | Fixed in the engine, commits `5c28141d` and `0ca656bd`, in two parts. Progress: from the moment a tool call's request arrives, a request with a progress token gets `notifications/progress` every 30 s, its progress counting up. While the call waits for its turn behind another (the bridge runs one call at a time; added by `0ca656bd`, since a call queued behind an export or a long test run could still meet the client's timeout) its message names the tool, how long it has been queued and the tool whose call runs; once it runs, it names the tool and how long it has run and, while `editor_launch` waits for a machine-wide lock, the wait's latest report (the lock, its holder's pid, worktree and command, the seconds waited and the bound), which the launcher hands over as the call's current activity; a request without a token gets none, and a notification that cannot be sent (bounded at 5 s) never fails the call. The bound: the launch's wait ends at 25 min, below the client's idle timeout, so a client that asks for no progress still hears `LockUnavailable`, whose hint says to call `editor_launch` again to keep waiting or, for the GPU lock, to launch with `renderer: "none"` when nothing renders. Architecture §2.3 and §13.8, ADR 0028 decision 16 and the skills describe it. The builder launched with `renderer: "none"`, since S3 needed no screenshot or export; nothing was worked around in the game |
| F15 | S4 | Minor. `test.run`'s case rows and JSON report do not say how many expectations a passing case evaluated, so a builder cannot see from the run that the checks it added ran. S4 proved its new checks with `script_eval` at the end of `Opening.replay` instead: decision 2's cell mapping matched all 200 well cells, an upside-down and a mirrored mapping none. Evidence: S4's report | – (new) | `test.run`'s report (`TestMethods`, `Testing`; no phase-3 owner) | Decided, not fixed in M25 (the lead's decision): a `Docs/Backlog.md` §6 row (a per-case count of the expectations evaluated and passed); the builder's proof was a `script_eval`, and nothing in the game works around it |

## Friction fixes

### F01: small text keeps its strokes

- **Symptom** (S1, the friction list's F01): in the default font at 9 and 10 scene pixels per em, strokes under a pixel wide dropped out of UI text drawn at the 360 × 450 reference and upscaled twice ("HOLD" read close to "IIOLD").
- **Root cause.** Not the sampling the lead suspected: the atlas is sampled bilinearly (the Text renderer's sampler filters linearly, at its one mip), and neither the Ui pass nor the Text pass snaps glyph quads (only the canvas's rectangles snap, `PixelSnap`). The pixel-perfect output only doubles what the scene image holds. The scene image itself lost the strokes, in `Passes/Text.slang`'s coverage: a smoothstep across the outline whose half-width was the field's screen-space derivative, `fwidth(field)`. At 9 pixels per em, Inter's stems (about 0.09 em) are about 0.8 pixel wide, and a 48-pixel atlas pixel is a fifth of a screen pixel. Where a stem falls between two pixel centres of one 2 × 2 quad, the field rises to the stem's middle and falls again between them, so the two centres sample about the same value, just outside the outline, and the derivative across the quad cancels to almost nothing. The smoothstep then became a hard step at the outline, and both pixels took no coverage at all: the stem vanished. Whether a stem survived depended on where it fell between the pixel centres and on how its two pixels paired into quads, which is why some stems of a word drew and others did not. From 12 pixels per em a stem is over a pixel wide, so a pixel centre always lies inside it and keeps at least half coverage whatever the derivative: those sizes read fine.
- **The fix.** The smoothstep's half-width is still `fwidth(field)`, but outside the outline never less than the field's step across one screen pixel where it is a distance: the outline's value over the font's `Spread` per atlas pixel (`TextConstants`, the Text program's new push constants, slot 1 beside `b0`, from the drawn font's `FontData::Spread`) times the atlas pixels one screen pixel spans, the smaller singular value of the atlas coordinates' screen-space Jacobian, and at most the outline's value, so the field's clamp to 0 never takes coverage. Where the field is a true distance `fwidth` is at least that, so such an edge draws as before. Across a thin stroke's middle, where the derivative cancels, the pixels on either side take the coverage of their distance to the stroke (about 0.4 each for a 0.8-pixel stem centred between them) instead of none. Inside the outline the floor is left out: a cancelled derivative there only sharpens the edge towards the full coverage those pixels showed before, so no pixel of any text takes less coverage than before, and every changed pixel moves towards the text's colour. (The floor on both sides, tried first, also lightened the middles of strokes one to two pixels wide in the larger goldens by up to 16 %; that is a change beyond the stems, so it was not kept.) Every text the shader draws gets it: UiText, the Text component (Screen and World), billboards and labels.
- **Regression test.** "TextRenderer: UI text and screen text of 9 and 10 pixels keep every stroke under a pixel-perfect camera at scale 2" draws "HOLD LEVEL LINES PRESS RESTART" and "SCORE NEXT ROTATE BACK" at 9 and 10 pixels per em as UiText in a Screen canvas and as Screen Text, under a 360 × 450 PixelPerfectCamera in a 720 × 900 view, and checks every stroke a row or a column of pixel centres crosses in each glyph (found in the font's own atlas field) for coverage of at least 64 of 255. Before the fix it failed (Debug, API 1.4: `CHECK(check.Misses.empty())`); with the fix it passes, with the rest of the Text, Ui pass, shader, pipeline, scene renderer, editor overlay, debug renderer and pixel-perfect camera cases (115 cases, Release, under API 1.4 and 1.3).
- **Goldens.** Ten goldens changed, each only in the thin strokes of its distance-field text; the other 29 match as before. Every changed pixel moved towards its text's colour (one pixel of `AnnotatedScreenshot` moved away by 1/255 in one channel, within the comparison's tolerance), and nothing else moved. They were regenerated with `--update-golden --filter` on exactly these ten cases, each candidate looked at beside its old image. Pixels changed of 230,400 (those beyond 2/255; the largest difference):
  - `AnnotatedScreenshot` 190 (160; 77/255): the entity labels ("Cube c48cc8", "Sphere 32a574").
  - `Combined2DUi` 738 (486; 43/255): the button's "Step" and the "Steps 0" counter, drawn at the 200 × 120 reference.
  - `DebugDraw` 77 (53; 51/255): the "Debug" label.
  - `Text` 991 (631; 71/255): the screen and world texts; "Bottom left, 24 px", 8 pixels per em, read "Rottom left, 24 px" before.
  - `Ui` 248 (100; 67/255), `UiWidgets` 371 (190; 46/255) and `UiWorld` 476 (321; 53/255): their labels.
  - `UiCjk` 1,461 (817; 78/255), `UiArabic` 738 (389; 45/255) and `UiHebrew` 857 (437; 67/255): the Latin captions and the thin strokes of the CJK, Arabic and Hebrew glyphs.

## Session record

At most three sessions; a fourth needs the product owner (decision 6). S4 ran by the lead's decision at the milestone review (decision 6, "Landing").

| Session | Date | Builder invocation | Reached | Friction ids | Backup, screenshots and what followed |
|---|---|---|---|---|---|
| S1 | 2026-11-11 | A background agent of the lead's session, with the prompt `bin/Reviews/M25-Builder-S1.md`; `editor_launch {project: "Projects/Tetris", create: true, template: "Empty", renderer: "vulkan"}`; about 96 tool calls of the 300 budgeted | The complete game, with all ten checklist items held. The replays, each recorded from its suite, verified twice with equal `stateHash` and failing with `withoutInput`: `Opening` `0213c7a651593710`, `LineClear` `96783a85ffdf5b86`, `Restart` `c0f515fbf2d033b5`. The 10,000-tick check ended at `edb3b90d20e81cbd`, with 245 entities, 0 bodies and 0 voices at ticks 5,100 and 9,600 | F01 to F09 | The backup in `bin/Reviews/M25-Backup/S1/`, the look screenshots (`Play`, `Clear`, `GameOver`, `Annotated`, `Editor`) in `bin/Reviews/M25-Look/S1/`. The look review: the board, the palette and the HUD as decision 4 specifies, except F01. The lead's scratch-index checks: decision 9's component check clean on the working tree; `AuditProvenance.py --project Projects/Tetris` exit 0 (56 engine-consumed files, 56 entries, 194 transcript lines, 97 requests, no warning); the games suite (Debug, Release, Dist) green, Tetris's editor halves clean with the three replays at their committed hashes and failing without input, its Dist half's `--feature-test` (5 suites, 3 replays) at the same hashes in both editors and the exported Dist and the plain Dist export's smoke test exit 0, Breakout and NativeTest unchanged and green; `Hardening.py --steps soak --soak-only "Tetris:*"` passed every rule over 10,000 frames (largest private memory growth 0.913 %, `Tetris:Assets/Scenes/Main.scene`; a warning only because the soak was partial). Then the friction fixes on `fix/m25-s1`: F01 (`f9509d61`), F02 (`7131d747`), and F08 with this triage's record; none changes Tetris's files or hashes |
| S2 | 2026-11-12 | A background agent of the lead's session, with the prompt `bin/Reviews/M25-Builder-S2.md`; `editor_launch` without `create`; 66 tool calls of the 150 budgeted | All ten checklist items held. L4: nine UiTexts overflowed (the labels 12.0996 in 12, the values 16.94 in 16, `GameOverTitle` 19.36 in 18), fixed by `UiRect` heights 13, 17 and 20 in one `edit_batch`, after which all 14 UiTexts report `textOverflows` false. The replays were re-recorded, because the heights are hashed data: `Opening` `00e57904fd23c567`, `LineClear` `57a04e6df7415162`, `Restart` `c661a4e2e2d7981a`, each verified twice and each failing without its input at checkpoint tick 60; the testing Dist export reported the same three. The 10,000-tick check ended at `b5905527b5d7bc67`, with 245 entities, 0 bodies and 0 voices at ticks 5,100 and 9,600. The Dist smoke test exited 0 | F10 to F13 | The look review after F01 and S2: every text legible, the board, the palette and the HUD as decision 4 specifies. The lead's scratch-index checks after S2: decision 9's component check clean on the working tree; `AuditProvenance.py --project Projects/Tetris` exit 0 (56 engine-consumed files, 56 entries, 310 transcript lines, 155 requests, no warning); the games suite (Debug, Release, Dist) green, Tetris's editor halves clean (`script.check`, `project.validate` and the upgrade dry run) with the three replays at their committed hashes in `test.run` and failing without input, its Dist half's testing export (5 suites, 3 replays) passing `--feature-test` with every replay at its committed hash in both editors and the exported Dist, and the plain Dist export's smoke test exit 0, Breakout and NativeTest unchanged and green; `Hardening.py --steps soak --soak-only "Tetris:*"` passed every rule over 10,000 frames (largest private memory growth -0.115 %; a warning only because the soak was partial). S2's triage needed no S3, by the lead's decision: it left nothing for automation to change in the game, since F10 to F13 are engine and documentation fixes. (S3 ran later for decision 9 step 5's review of the staged rebuild, below.) Then the friction fixes on `fix/m25-s2`: F10 and F11 (`2dd34dc4`), F12 (`597cd1a5`), and F13 with this triage's record; none changes Tetris's files or hashes |
| S3 | 2026-11-13 | A background agent of the lead's session, with the prompt `bin/Reviews/M25-Builder-S3.md`; `editor_launch` without `create`, first with `renderer: "vulkan"`, which hit F14 (no answer for 1,800 s), then with `renderer: "none"` (S3 needed no screenshot or export); 37 tool calls of the 150 budgeted. Why S3 ran: decision 9 step 5's review of the staged rebuild (0 blocker, 0 major, 1 minor, 2 nits) found two gaps in the game's own test suite and one in `AGENTS.md`, which only automation (or, for `AGENTS.md`, the builder) may change, and decision 6 allows S3 for the milestone review's findings on the game | The review's findings fixed. The new `Tetris` case "the next boxes show the queued pieces centred in their boxes" (decision 8). The pool case now checks that the 200 well cells are active, with a `SpriteRenderer` and no `MeshRenderer`, and that the 14 HUD texts are `UiText` with no `Text` component. `AGENTS.md` lists `.luaurc` among the engine-consumed files and records S3. `test_run` 39 of 39 (19 `Board`, 14 `Tetris`, 3 recording suites, 3 replays). The replays are unchanged, each verified once with `strictHash`: `Opening` `00e57904fd23c567` (5 checkpoints), `LineClear` `57a04e6df7415162` (1), `Restart` `c661a4e2e2d7981a` (1). `script_check` (12 scripts), `project_validate` and the upgrade dry run (43 files) clean; `editor_shutdown` exit 0 | F14 | The backup in `bin/Reviews/M25-Backup/S3/` (60 files: the committed project at `17e687da`, laid out like S1's and S2's). No screenshots: S3 changed no look (only the game's test suites and `AGENTS.md`, no scene, prefab, script or setting that renders), so, by the lead's decision, S2's look review (`bin/Reviews/M25-Look/S2/`) stands for the final game. The lead's checks after S3 and the replacement commit (decision 9): the staging, 5 added, 23 deleted (the 3D build's materials), 33 modified and 3 renamed; the component check clean; `AuditProvenance.py --project Projects/Tetris` in index mode exit 0 (56 engine-consumed files, 348 transcript lines, 174 requests, 0 warnings); the games suite in Debug, Release and Dist green (Tetris's three replays at their committed `FinalStateHash` in the Debug and Release editors and the exported Dist, each failing without its input; the testing Dist export's 5 suites; the plain Dist export's smoke test exit 0; Breakout and NativeTest green too); the replacement commit `17e687da` "M25: Tetris rebuilt in place as pixel art (sessions S1 to S3)"; the fresh-clone audit (a depth-1 clone of `milestone/m25` into an empty scratch directory outside the repository) exit 0 with the same counts; then `git clean -fdX -- Projects/Tetris`, `D:\Engine` back on main and the M25 slot re-attached to `milestone/m25`, which closed the in-place window on 2026-11-13. Then the friction fixes on `fix/m25-s3`: F14 (`5c28141d`, and `0ca656bd` for a queued call), and this record; none changes Tetris's files or hashes |
| S4 | 2026-11-13 | A background agent of the lead's session, by the lead's decision at the milestone review (decision 6); `editor_launch` without `create`, with `renderer: "none"` (S4 needed no screenshot or export); about 25 tool calls. Why S4 ran: the milestone review's one finding on the game, that the `Tetris` suite never checked where the well cells or the hold preview's cells are ("Milestone review"), which only automation may change | The finding fixed in `Assets/Tests/Tetris.test.luau` (one `script_write`): the pool case checks every well cell's `Transform` translation at exactly (column − 5.5, row − 10.5, 0), all 200 cells (`expectWellInPlace`); the NEXT boxes' helper became `expectPreview(game, slot, boxName, piece)` (slot 0 the hold, 1 to 3 the NEXT boxes), and the hold case, after the first hold of the known O (it now also expects the first piece to be the O), checks four active children of `HoldBox` at (24, 8), (40, 8), (24, 24) and (40, 24), 16 × 16, `Ui.GetRect`'s layout relative to the box agreeing. `AGENTS.md` by hand. `test_run {}` 39 of 39 (19 `Board`, 14 `Tetris`, 3 recording suites, 3 replays); `script_check` (12 scripts) and `project_validate` without diagnostics. No hashed value changed, so the replays kept their hashes, each verified once with `strictHash`: `Opening` `00e57904fd23c567` (5 checkpoints), `LineClear` `57a04e6df7415162` (1), `Restart` `c661a4e2e2d7981a` (1); `editor_shutdown` exit 0 | F15 | The backup in `bin/Reviews/M25-Backup/S4/` (60 files, a `git archive` of `cd055176`). No screenshots: S4 changed no look, so S2's look review stands. The lead's review of S4's diff approved it, with one nit fixed by the lead (an over-long `AGENTS.md` line rewrapped; `AGENTS.md` is not engine-consumed). The lead's checks on the staged files, all green: decision 9's component check clean; `AuditProvenance.py --project Projects/Tetris` in index mode exit 0 (56 engine-consumed files, 386 transcript lines, 193 requests, 0 warnings); the games suite in Debug, Release and Dist (Tetris's three replays at their committed hashes in the Debug and Release editors and the exported Dist, each failing without its input; the Dist smoke test exit 0; Breakout and NativeTest green). Then S4's checkpoint commit `cd055176` "M25: S4, the Tetris suite checks where the cells are" on `milestone/m25` (only `Projects/Tetris`: `AGENTS.md`, `Assets/Tests/Tetris.test.luau`, the transcript and provenance), the fresh-clone audit (a depth-1 clone of `milestone/m25`) exit 0, `D:\Engine` back on main and the M25 slot re-attached to `milestone/m25`; `fix/m25-landing` was rebased onto it before the review's final runs |

## Landing

Decision 10, in the M25 slot, on 2026-11-13.

- **The merge** (step 1). `milestone/m25` merged main `d30d2ce2` (the phase 3 setup, ADR 0041's script-check fix among it) in `381805d0`, "Merge main into milestone/m25 (the phase 3 setup)". Three documents changed on both sides, `Docs/Architecture.md`, `Docs/Backlog.md` and ADR 0028, and git merged each on its own: no conflict, and nothing under `Projects/Tetris`, which main never touched.
- **The redo** (step 2), done:
  - **Registries.** M24 has not landed, so main brought no sibling's entries, and M25's friction fixes added no method, binding, validation code or shared-list entry: nothing to append.
  - **Main's canonical upgrades on the rebuilt Tetris.** None needed: between the merge base `c36b0315` and `d30d2ce2` main changed nothing under `Projects/` or `Tests/Projects/`, Breakout's and NativeTest's `Automation/BuildLog.jsonl` gained no `cli` lines, and main's `ScriptImporter` change (ADR 0041) bumps no importer version.
  - **Generated files and builds.** `Generate.py`, Debug, Release and Dist built, and `GenerateDocs.py --check --config Debug` passed on the merged tree: the merge left the references unchanged.
  - **FeatureTest and fixtures.** No friction fix touched FeatureTest, so no `Scaffold.py` ran; no fixture generator changed with the merge.
- **The review** (step 4): the next section. Its fixes are one commit on `fix/m25-landing`, which the lead moves `milestone/m25` to; its one finding on the game went to builder session S4 (the session record), whose test-only checkpoint commit `cd055176` the lead made on `milestone/m25`, onto which `fix/m25-landing` was rebased before the review's final runs. S4 changed no hashed value, so the three replays keep their hashes and M26 still needs no Tetris re-record (decision 2).
- **Then** the gate (step 5), the soak (step 6) and the fast-forward (step 7), which the lead records under "Before milestone completion".

### The soak

Step 6 on the gated tree (`9d96ed93`, `CI.py --require-clang-cl` green): `Hardening.py --steps soak`, 65 subjects of 10,000 frames, failed three times, each time on the private memory rule (the median private bytes of 0.50 to 0.55 against the maximum of 0.95 to 1.00, below 1 %) and each time in other FeatureTest subjects, while every subject's counters (the device's allocation count, entities, bodies, voices, the script heap) stayed flat:
- **Run 1** (alone, under the GPU lock, beside other worktrees' builds): `SceneLoadB` 4.51 % (one +14.8 MiB step at 0.70, flat after) and `TonemapPbrNeutral` 1.38 % (+2.3 and +2.1 MiB at 0.95 and 0.975).
- **Run 2** (alone): `TonemapAces` 4.28 %, `Physics2D` 4.27 % and `UiHebrew` 3.66 %, each one late step.
- **Run 3** (under the gate, GPU and uncapped build locks, as `CI.py` holds them; subjects 20 to 63 ran at about twice the usual 40 s): `UiTextInput` 4.12 % (one +13.5 MiB step at 0.80).
- **A targeted re-soak** (`--soak-only` of `SceneLoadB`, `TonemapPbrNeutral`, `UiWidgets` and `PhysicsSemantics`) passed, the largest growth 0.051 %, its steps at 0.15 to 0.2. Phase 2's passing hardened soak (M23, largest 0.18 %) shows the same one-off step in 36 of its 65 subjects, 33 of them at 0.2.

**The investigation.** Editors started exactly as the soak starts them (`soak_editor_scene`), sampled about every 0.16 s from outside by a scratch script (private bytes and working set, every thread with its start address and cycle count, the modules, the committed-region map diffed across each step and the content of each grown region), soak-faithful and idling with the scene open before `play.start`, and `--renderer none` editors as controls. Two causes:
- **Cause 1, the NVIDIA driver** (`SceneLoadB`, `TonemapAces`, `Physics2D`, `UiHebrew` and `UiTextInput`): the step of about 12 MiB (10.6 to 16.6 MiB in the reports, the working set 8 to 14 MiB more) is the driver compiling on its own worker thread (the start address `nvoglv64.dll+0xa3b690`, busy only then). An allocator region whose header points into `nvgpucomp64.dll`, NVIDIA's shader compiler, and holds LLVM's x86 target-feature names grows by 9.6 to 11.3 MiB, and one whose header points into `nvoglv64.dll` by 1.7 to 2.4 MiB; no module or thread is added, no first-party counter moves and the editor logs nothing. The working set's excess is consistent with the compiler's code being paged in (`nvgpucomp64.dll` maps 110 MiB). It came 10.6 s and 13.7 s after launch in two soak-faithful probes, 24.5 s after launch in an editor idling with its scene open before any `play.start`, and not at all in 64 s of a `UiTextInput` probe; in the soak reports it lands about 11 to 14 s after launch on a quiet machine (phase 2's steady 0.2) and up to about 48 s under load, at whatever tick that is (run 3's slowed subjects at 0.10 to 0.125). M15's recorded stream-C soak (`Tests/Data/Hardening/SoakStreamC.json`, an earlier driver, before the game view) shows the same step in 5 of its 11 subjects at 0.35 to 0.4. The Runtime subject never shows it (four runs: only +2 to 3 MiB in its first 5 %, its startup). The driver decides when, and whether, it runs. The options the lead weighs, each with what it would and would not still detect: measuring the editor's own heap instead of the process's private memory (a `stats.get` field; a leak inside the driver's heaps goes unseen); a longer gate soak (`--soak-frames` 30,000 to 40,000, the rule unchanged, three to four times the step's time and a larger hardening bound); a rule that allows one bounded step that stays flat (a one-off retention after the baseline goes unseen); one re-soak of a failing subject (a one-off goes unseen; costs time on a failure); a warm-up before `play.start` (not reachable: the compile came 4 to 48 s in, or never). The product owner chose the first, option A, on 2026-11-13, and the lead refined it to A-prime the same day (below).
- **Cause 2, first-party: the job workers' profiler rings** (`TonemapPbrNeutral` in run 1, and part of the warm-up's growth). A thread's ring of `Core/Profiler` (`ZonesPerThread`, 65,536 zones of 32 bytes: 2 MiB) was allocated at the thread's first zone, a JobSystem worker's at its first job (`ENGINE_PROFILE_SCOPE("JobSystem.Job")`). The editor's hot-reload polls (every 0.5 s of frame time) reach its 22 workers in the order the operating system wakes them, so their rings arrived through the warm-up (about +16 MiB a sample at 0.025 to 0.075) and some late: run 1's `TonemapPbrNeutral` failed on two (+2.3 and +2.1 MiB at 0.95 and 0.975), and the reports hold other late +2.0 MiB steps. An idle editor held 23 private blocks of 2 MiB whose first slot names `JobSystem.Job` (22) and `FrameLoop::Events` (1).

**The fix of cause 2.** `Profiler::SetThreadName` registers the calling thread's ring at once when it has none of the current initialization, outside Dist; Dist records no zone, so there it only keeps the name and its footprint is unchanged. The `JobSystem` constructor returns once every worker has named itself (`Job <n>`), so every worker's ring is allocated when the system starts, before any job, and never at a moment the scheduler picks. `ZonesPerThread`, the ring's size, the profiler's thread safety and a capture's `ThreadZones` (registration order) and thread names are unchanged. The regression tests, each run alone without the fix and failing: "Profiler: a thread that names itself holds its ring before it records a zone" (without it the named thread had no ring) and "JobSystem: every worker holds its profiler ring before any job runs" (without it no worker had one). `Core/Profiler` and `Core/Jobs/JobSystem` have no phase-3 owner: ADR 0028's amendment "M25's soak" records them. `Docs/Backlog.md` §3's row of cause 1 is closed by the fix below, and §6 holds the limit that fix accepts.

**The fix of cause 1: the soak measures the C runtime's heap** (A-prime, the lead's refinement of the product owner's option A, 2026-11-13). The driver's background compilation commits most of its memory in heaps of its own, at a moment the driver picks, and no first-party code makes or times it; every first-party allocation, and that of every library allocating through the C runtime (Luau, Jolt's default allocator, Dear ImGui, NVRHI's host objects), is in the heap the C runtime allocates from. So an editor subject's memory rule measures that heap, and the private memory it no longer judges stays visible as a warning:
- **The measure.** `ProcessHeap::GetCommittedBytes` (`Platform/ProcessHeap.h`): `HeapSummary` on the process heap, which the Universal CRT allocates from ("ProcessHeap: the C runtime allocates from the process heap" compares it with `_get_heap_handle` through the tests' platform probes), its committed bytes counting the heap's large blocks, which have memory of their own ("ProcessHeap: a large block of the process heap raises the committed bytes and its release lowers them", 8 MiB through `HeapAlloc`, and "ProcessHeap: a large allocation raises the committed bytes and its release lowers them", through the C runtime). A Windows backend only (ADR 0028 decision 10): Linux and macOS report `Unsupported`. In the hardening stage's AddressSanitizer build the sanitizer's runtime serves `malloc`, `operator new` and `HeapAlloc` itself from its own allocator, whatever heap a `HeapAlloc` names, so there the measure leaves those allocations out and the process heap holds about 0.3 MiB ("ProcessHeap: the AddressSanitizer build allocates outside the process heap"); the soak never runs on that build.
- **`stats.get`** reports it in both hosts as `processHeapAvailable` and `processHeapCommittedBytes` (approximate bytes, as the script heap's), beside the other memory fields; false and 0 where the platform has no measure ("StatsGet: reports the committed bytes of the C runtime's heap where the platform measures them", `test_editor_ui.py`'s editor case and `test_runtime_render_stats.py`'s Runtime case). The editor's Stats panel does not show it: its Memory section is in M24's editor-UI golden `EditorDesignSupporting1600x900`, and a heap that differs on every run would make that image change with each run; parity (Roadmap rule 5) asks a method of every editor-visible capability, which `stats.get` is.
- **The proof.** The probe of the investigation, now also reading `stats.get` after every step (and every 0.5 s while idle), on the built editor. An editor idling with `Callbacks.scene` open met the driver's compilation 11.5 s after launch (its private memory +9.7 MiB, its working set +20.5 MiB, the driver's thread busy), and the heap read 77.840 MiB just before and just after it; in soak-faithful runs of `Callbacks.scene` and `SceneLoadB.scene` the compilation came at ticks 2,000 and 1,750 (+16.8 and +12.4 MiB private), and the heap, which grows by about 1 MiB a step through the warm-up (the first-party memory that fills, the profiler's GPU-zone ring among it), stayed within 0.008 MiB of 113.42 MiB and at 111.430 MiB from tick 5,000 to the end. But the driver also allocates part of the compilation from the process heap, which those runs hid only because the heap had committed room to spare. Two other probes saw the heap grow with the step: by a new large block of 1.57 MiB in one, and in the other by 1.12 MiB of a heap segment that held the shaders' binding names (`Instances`, `MaskDraw`, `Material`, `BaseColorMap`, `GtaoConstants`, `tViewDepth`, `tSceneNormals`, `SourceOcclusion`). And in two targeted soaks of the six failed subjects under option A as first specified (the heap's growth below 1 % of the heap), each of the eight driver steps after 0.1 (+10.9 to +14.6 MiB private) raised the heap: by 0.76, 0.28, 0.20 and 0.55 MiB in the first, and by 1.82, 1.91, 1.58 and 1.00 MiB in the second. The heap is 109 to 137 MiB in these editors, about a third of the private bytes, so 1 % of it is only 1.1 to 1.4 MiB: the second soak failed `Physics2D` at 1.16 % of its heap (+1.58 MiB beside a +14.6 MiB private step at 0.65), 0.42 % of its baseline private bytes. The rings count in the heap at startup: a temporary unit case measured the heap 16.08 MiB higher right after `JobSystem(8, ...)` returned (8 rings of 2 MiB), and an idle `--renderer none` editor held its 23 rings of 2 MiB a moment after launch, none of them yet holding a zone, its heap flat at 63.46 MiB over 25 s of 0.5 s readings, where before this landing its rings arrived one 2 MiB block a poll for the first 14 s.
- **A-prime**, the lead's refinement of the product owner's option A (2026-11-13), on that proof: the rule measures the heap's committed bytes, and its growth in bytes (the final window's maximum less the baseline window's median) must stay below 1 % of the subject's baseline private bytes, in the same windows. That is the absolute bound the private memory rule had (about 3.5 to 3.8 MiB for these editors), so the soak keeps its sensitivity to a first-party leak, while the driver's private step outside the heap no longer counts and the share it allocates from the heap (0.2 to 1.9 MiB) stays within the bound; 1 % of the heap itself is too tight for that share. It stays within option A: the rule measures the engine's own heap, and the private bytes are a warning. Under it the two targeted soaks' largest growths were 0.287 % (`SceneLoadB`, the first) and 0.420 % (`Physics2D`, the second) of the baseline private bytes. One transient was observed, not a failure: in the first, `SceneLoadB`'s heap rose 2.04 MiB at 0.55 and fell 1.01 MiB at 0.60 (its private bytes +1.26 and -1.01 MiB), outside any driver step, and stayed 1.04 MiB above its baseline to the end; it did not recur in the second, and no thread names itself in the middle of a soak, so it is no profiler ring. Nor is Tetris's `Main.scene` step of about 2 MiB at 0.575, which this record first counted among the rings: it came in M25's three failed runs and again in the full soak after the rings' fix (+2.38 MiB private), where only 0.38 MiB of it was in the heap, and a ring is a 2 MiB block of the heap; its private growth stayed below 1 % (0.87 %).
- **The rule** (`Scripts/Lib/hardening.py`'s `evaluate_soak`, the same windows and limit): for an editor subject whose every sample carries the heap, the heap's growth in bytes stays below 1 % of the subject's baseline private bytes, and its failure line names that growth, the bound and the baseline private bytes the bound derives from; a subject without baseline private bytes fails, naming it. Its private memory's growth of 1 % or more is a `WARNING` line naming the subject and its window values, kept in `Soak.json` and the JUnit report and counted in the step's summary, and fails nothing (a warning status still means a partial soak). The Runtime subject, which has no automation, and an editor without the measure keep the private memory rule. A subject with the heap in some samples only fails, naming how many have it, rather than mix two measures or drop samples. `Soak.json` is version 2: each sample's `heap_committed_bytes`, each subject's `Measure`, `Growth` (the measure's growth over the baseline private bytes), `PrivateGrowth` and `Warnings`. Tests in `Tests/Automation/test_hardening.py`: a private step beside a flat heap passes with one warning; heap growths of 3,599,000, 3,600,000 and 3,601,000 bytes beside 360,000,000 private bytes pass, fail and fail, with a heap of 120,000,000 bytes; the second soak's `Physics2D` (1.16 % of its heap, 0.42 % of its private bytes) passes with its warning; a heap-measured subject without baseline private bytes fails; the Runtime subject still fails on private growth; a subject with the heap in half its samples fails naming them; the step prints and counts the warning without changing its status; and the report's version 2 fields. Each failed against the scripts before it: option A's cases against the previous scripts (`SoakSample` took no heap, `SoakResult` had no measure, the report was version 1), and A-prime's three against option A as first specified (the 3,599,000-byte growth and `Physics2D` failed at 3.00 % and 1.16 % of the heap, and the subject without private bytes passed).
- **The runs** on the final tree: the Debug, Release and Dist builds without a warning; `Lint.py`, strict, without a finding; `Format.py --check`; `GenerateDocs.py --check --config Debug`; `CheckRuntimeMap.py --config Dist`; the full Debug unit suite (4,728 cases passed, the 40 child-process targets skipped) and the touched cases in Release; the `DocExamples` cases for the edited skills; the automation suite's stats and hardening tests (`--filter`). Then, on the committed tree (`998b9e3b`): two targeted soaks of the six failed subjects, each passing all six with no warning (the largest growths 0.003 % and 0.093 % of the private bytes; their eight driver steps, at 0.175 to 0.5, before or in the baseline window, moved the heap by -0.09 to +1.80 MiB), and one full soak under the gate (gate, GPU and uncapped build locks, as `CI.py` holds them), all 65 subjects passing in 2,842 s: the largest growth 0.484 % of the private bytes (`Scripting`, whose driver step at 0.70 added 14.1 MiB of private memory and 1.62 MiB of heap, 1.46 % of its heap, which option A as first specified would have failed, as it would `UiWidgets` at 1.03 %), three private warnings (`UiWidgets` 3.90 % and `Scripting` 4.46 %, each the driver's late step, and `GltfFixture` 1.14 %, a +3.4 MiB private step at 0.70 with 0.21 MiB of it in the heap, observed and not attributed), and the Runtime subject's private growth -5.39 %.
- **The fix's review** (2026-11-13): an independent review of the three commits found 6 findings (1 major, 2 minor, 3 nits), each fixed by the lead's decisions. The major one: in the AddressSanitizer build the C runtime allocates outside the process heap, so the C-runtime cases of `ProcessHeapTests.cpp` would fail the hardening stage's sanitized unit run; they now branch on `__SANITIZE_ADDRESS__`. Outside it a probe that allocates on the process heap itself (`Test::AllocateOnProcessHeap`) proves the large-block counting beside the C runtime's case; in it one case checks that neither a C runtime block nor the probe's reaches the process heap, since the sanitizer's runtime intercepts `HeapAlloc` too: run there, the probe's 8 MiB block left the heap at 319,488 committed bytes, so the review's proposal, the probe in every build, could not hold in that build. `stats.get`'s round trip bounds the heap by more than 0 there. The others: the skill, §3's Backlog row and ADR 0028's bullet state the warning's 1 % threshold and the private rule of an editor without the measure; tests read the warning lines in `Soak-Release.xml` and the warning count in a failed and a timed-out step; a passing subject's warning line says it warns at 1 % or more instead of naming a limit; `evaluate_soak`'s docstring says the mixed-measure failure names how many samples have the heap; and `processHeapAvailable`'s description covers a failed query. None changes what the rule decides.

## Milestone review

One review pass of the milestone diff (`d30d2ce2...381805d0`, 109 files: this record, the documentation checkpoint, the friction fixes, the rebuilt game and the merge), in three lenses: correctness and robustness; determinism, formats and integration; tests, parity and documentation. They reported 17 findings, 14 distinct (three were reported by two lenses): 0 blocker, 1 major, 7 minor, 6 nits. Each was checked against the code first, and each held, so none was rejected: 13 are fixed in the review's commit, and 1, on the game's own test suite, by builder session S4.

| Finding (lens) | Severity | Resolution |
|---|---|---|
| Architecture §13.11's walkthrough still describes M17's 3D build (tests; determinism) | Major | Fixed: §13.11 rewritten for the pixel-art build as decisions 1 to 4 and 8 built it (the settings with `SortingLayers`, `Ui.Navigation` false and the fixed window; the sounds and the Blocks sheet; the scaffold with the PixelPerfectCamera, the frame bars, the Screen-canvas HUD and the unfocusable RESTART button; the bevel Cell prefab; the scripts and the one-restart-per-tick rule; the 720 × 900 kept captures and `ui_tree`'s text boxes; the three replays with their committed hashes and decision b's restart-ending recording), the layout line's "Tetris/ (M17; rebuilt in M25)", and the `TetrisGame` prelude (`Tests/Data/Generate/MakeScriptFixtures.py`, regenerated) for the section's real `OnUiClick` and `OnFixedUpdate` excerpt, which the `DocExamples` cases type-check and lint |
| A cancelled `editor_launch` keeps waiting for the lock, holds the bridge, then spawns a rendering editor nobody asked for (correctness) | Minor | Fixed inside `Tools/MCP` (the lead's decision 1; `Scripts/Lib/locks.py` unchanged): the launch's wait tries the lock with locks' no-wait acquisition every quarter second (`WAIT_POLL_SECONDS`) and checks the call's cancel event between tries, which the server sets when the call's scope is cancelled, since the worker thread itself ignores the cancellation. A cancelled wait raises `LaunchCancelled`: nothing is spawned, a lock taken as the call was cancelled is released (a hold under an ancestor's releases nothing of it), and the bridge serves the next call at once. Regression cases in `Tools/MCP/tests/test_progress.py`: "a cancelled launch stops waiting for the lock and spawns nothing" (without the fix the next call, the hint's `renderer: "none"` launch, waited out the cancelled launch's wait, in both protocol eras) and "a launch cancelled as it takes the GPU lock releases it and spawns nothing". Architecture §2.3 and §13.8 and game-building's launch paragraph say so |
| The progress message's seconds waited are the lock's latest report's, up to 300 s stale (correctness) | Nit | Fixed (the lead's decision 2): the wait is the launcher's activity as a `LockWait` (the holders' text from the lock's report, the wait's start and its bound), and each progress message counts the seconds when it is sent. `test_progress.py`'s lock-wait case now waits for a later message to count more (without the fix every message said 0 s), and "a lock wait counts its seconds when it is read" checks the text |
| The `includeInactive` case never compares the play session's state hash (determinism) | Minor | Fixed (the lead's decision 7): the F10 case reads `play.state`'s tick and `stateHash` before the two play-target `ui.tree` calls, plain and `includeInactive`, and checks both unchanged after them |
| This record's completion list leaves done items unticked, and the landing section is missing (tests) | Minor | Fixed: "Landing" and this section, and the done items of "Before milestone completion" ticked with their evidence |
| `ui.tree`'s `textBounds` repeats the Ui pass's text placement and font chain with no test tying them (tests) | Minor | Fixed by a test (the lead's decision 4). The placement (`PlanGlyphs`) and `BuildUiFontChain` live in `Renderer/UiPass.cpp`, the font and text pipeline, which phase 3's ledger leaves in its "none" row outside M25's named text fixes (`TextLayout`, `TextRenderer`, the sampling in `Text.slang` and `Quad.slang`), so the copy stays in `UiMethods`, and "UiMethods: ui.tree's text boxes are where the Ui pass lays the texts out" compares it with what `PlanUiPass` draws, through public API: left-, centre- and right-aligned texts at the top, middle and bottom of their rectangles, a wrapped text, a text in a font that falls back to the Default font for part of it, and a focused text input whose caret moves its long text inside its field. Run alone against a `ui.tree` that ignored the fallbacks and the caret's shift, it failed at the fallback text and the focused input |
| `ui.tree`'s new outputs are untested in the exported Runtime (tests) | Minor | Fixed (the lead's decision 5) where the Runtime's `ui.tree` is tested, `Tests/Automation/test_ui.py`'s Runtime case on the Release export: the Runtime reports every element's scale and `active`, a positive text box, and with `includeInactive` an inactive element marked inactive, and at the editor's resolution every entry's rectangle, scale, `active`, `textBounds` and `textOverflows` equal the editor's. The Dist Runtime holds no automation (`CheckRuntimeMap.py`), and FeatureTest's runs do not call `ui.tree`, so neither changes |
| F01's per-font `Spread` push constant is untested: every font drawn has spread 8 (tests) | Minor | Fixed (the lead's decision 6): "TextRenderer: glyph runs and screen texts are shaded with the Spread of their own font, batch by batch" draws flat-field fonts (a uniform atlas just outside the outline, so only the shader's floor shades them) of spreads 4, 16, 8 and 4 again as glyph runs, and of spreads 4 and 8 as screen texts, each at the coverage its own spread gives (81, 0 and 40 of 255). Run alone against a Text pass that pushes a constant 8, it failed: every glyph and text drew at 40 |
| The `Tetris` suite never checks where the well cells or the hold preview's cells are (tests) | Minor | Fixed by builder session S4 in `cd055176` (the lead's decision 9; the session record): the pool case checks all 200 cells' translations, the hold case the hold slot's four cells in `HoldBox`; test-only, so the replays keep their hashes |
| game-building's Definition of done still calls the 3D build the committed `Projects/Tetris` (determinism; tests) | Nit | Fixed: the 3D build "stays in git history, replaced in place by the committed pixel-art build"; the skill stays under `docs.get`'s inline limit |
| The lead's decision b says `Restart.replay`'s checkpoints prove the top-out, but its one checkpoint follows the restart (determinism; tests) | Nit | Fixed: decision b and decision 8's row say exactly what its checkpoint and final hash prove (the top-out and the shown panel only indirectly) and that the `Tetris` suite's top-out cases check the game-over state; §13.11 says the same. No re-record |
| The friction list does not name the ledger basis of F08's `AssetMethods.cpp` change or of F01's `TextVertex.h`, and M26 rewrites the same `asset.create` sentence (determinism) | Nit | Fixed: F01's and F08's rows name their basis, and ADR 0028's amendment "M25's milestone review" records both, tells M26 to merge F08's sentence at its landing and add `Heightfield` to the types that take no values, and records M25's edit of decision 16's T-LOCKS text (F14's 25-minute wait), which the determinism lens noted |
| The Backlog's status line does not record M25, which adds §6 rows (tests) | Nit | Fixed: the status line names M25 and its §6 rows (F03 to F07, F09 and S4's F15) |
| F08 documents that `asset.create` refuses values for a Prefab, a Scene and a Folder, but no test asserts it (tests) | Nit | Fixed: "AssetMethods: asset.create refuses values for a Scene, a Prefab and a Folder and writes nothing" (each refused at `/values` with no file and no undo step; without values, or with null ones, each is created) |

**The lead's decisions** (2026-11-13, binding on the review's fixes): 1. the cancelled launch is fixed inside `Tools/MCP` if the lock library's public API allows it (it did); 2. the heartbeat counts the seconds waited when it is sent; 3. §13.11 is rewritten in its existing style, with the `TetrisGame` prelude; 4. `ui.tree`'s copy of the Ui pass's placement is shared with the Ui pass only if M25 may change the Ui pass's file, and tested against it otherwise, its caret and fallback paths included (it is tested); 5. the Runtime's `ui.tree` coverage is extended where it is tested; 6. the per-font spread gets a test that fails with a constant 8; 7. the play-scene case compares the session's tick and hash; 8. the nits as proposed, with ADR 0028's amendment bullet; 9. the game's test-suite finding goes to builder session S4, a deviation from decision 6's S3 reserve, which S3 spent; 10. main is not merged again in this step.

## Before milestone completion

- [x] Before session 1:
  - this record and the documentation checkpoint are committed;
  - the slot is detached, and `D:\Engine` is on `milestone/m25`, rebuilt, with the references check clean;
  - the reuse copy is made;
  - `Projects/Tetris` is emptied with the index intact, verified as decision 5 lists.

  Done on 2026-11-11: this record (`d1399e09`) and the documentation checkpoint with the lead's decisions before session 1 (`60ca234b`) committed; S1 then ran in `D:\Engine` on `milestone/m25` with its Release build, from the emptied folder, its transcript beginning at line 1 with `project.create`, and reused the board logic from `bin/Reviews/M25-Reuse/` through `script_write`, while the index kept the 3D build until the replacement commit `17e687da` staged the rebuild over it (the session record's S1 and S3 rows).
- [ ] After each session: the backup is in `bin/Reviews/M25-Backup/S<n>/`, the screenshots are in `bin/Reviews/M25-Look/S<n>/`, the friction log is stored, and the triage, the look review and the scratch-index runs are recorded. S4 (2026-11-13): the backup is in `bin/Reviews/M25-Backup/S4/`, no screenshots were taken since S4 changed no look (S2's look review stands), its friction F15 is triaged in the friction list, and the lead's checks are in the session record's S4 row.
- [x] Every friction item is closed or decided; every predicted item has its "Hit as"; the lead signed off the look review on the last session's screenshots. Done on 2026-11-13: F01 to F14 each fixed or decided (F14 by `5c28141d` and `0ca656bd`), and S4's F15 decided by the lead at the milestone review (a Backlog §6 row); P01 to P26 each with its "Hit as"; the look review signed off on S2's screenshots (`bin/Reviews/M25-Look/S2/`), which stand for the final game by the lead's decision, since S3 changed no look (the session record's S3 row).
- [x] The replacement commit is one checkpoint in `D:\Engine` with decision 9's checks green, including the fresh-clone audit; `D:\Engine` is back on main and the slot re-attached. Done on 2026-11-13: `17e687da`, after the component check, the index-mode audit and the games suite in Debug, Release and Dist, then the fresh-clone audit exit 0, `git clean -fdX -- Projects/Tetris`, `D:\Engine` on main and the slot on `milestone/m25` (the session record's S3 row).
- [x] Main is merged after the last session, with Tetris on M25's side and decision 10's redo done. Done on 2026-11-13: `381805d0` merged main `d30d2ce2` with no conflict and nothing under `Projects/Tetris`; the redo had nothing to append, no upgrade of Tetris to redo, and the references check passed on the merged tree ("Landing").
- [x] One review pass of the milestone diff, with every finding fixed. Architecture §13.11, the skills and the Backlog describe what landed. Done on 2026-11-13: 14 distinct findings, 13 fixed in the review's commit on `fix/m25-landing` and 1 by builder session S4 ("Milestone review"); §13.11 rewritten for the pixel-art build, game-building's Tetris and launch paragraphs current, and the Backlog's status line and its §6 rows (F15's added).
- [ ] `CI.py --require-clang-cl` is green on the final tree, then `Hardening.py --steps soak` on the gated tree.
- [ ] Main is fast-forwarded to `milestone/m25` before M26 lands.
