# FeatureTest

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done; `skills/luau-gameplay` and
`skills/game-debugging` cover scripts and diagnosis, and `reference/Components`, `reference/Automation`,
`reference/Validation` and `reference/FileFormats` the engine's data.

- The editor writes every engine-consumed file (`FeatureTest.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`. Do not edit those files by hand. FeatureTest has
  no `Automation/BuildLog.jsonl`: its content comes from batch scaffolds run with `Editor --batch` (below), never through
  the MCP bridge, which is what appends that log. `Automation/Provenance.json` has an entry for every file below
  `Assets/` and for `FeatureTest.eproj`: the scaffolds' runs record theirs, and the kept golden content's come from
  `Scaffold/Baseline/Provenance.json`, the record of `Scaffold/Golden.jsonl`'s run.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- FeatureTest is the engine's coverage project (`Docs/Architecture.md` §15.5 and §15.6), not a game. It sets
  `Testing.CoverageGates`, so an unfiltered `Editor --headless --project Projects/FeatureTest --run-tests` (or `test.run`)
  evaluates the six per-mode coverage gates and fails on any uncovered component, field, enum value, script API, callback,
  proxy field or source format (`python Scripts/Test.py --suite feature`).
- Its content is produced by the committed batch scaffolds under `Scaffold/` (`Editor --batch`), never by hand.
  `Scaffold/Scaffolds.json` lists them in run order, each with its id seed (`Editor --id-seed`) and renderer:
  `Settings.jsonl` writes the project settings, the stream scaffolds write the scenes, scripts, suites and test assets
  under `Assets/Scenes/`, `Assets/Scripts/`, `Assets/Prefabs/`, `Assets/Formats/`, `Assets/Localization/` (M22's string
  table), `Assets/Ui/` (M22's font imported with `Ranges` in `Assets/Ui/Fonts/`, and in `Assets/Ui/FallbackFonts/` the
  Hebrew font and the copy of Inter whose `Fallbacks` names it, which `UiFallback.jsonl`'s scene draws, M22's milestone
  review) and `Assets/Tests/<Area>/`,
  `Suites.jsonl` writes `Testing.Suites` and `Testing.Replays`, and `Replays.jsonl` records `Input.replay` and
  `Determinism.replay` last (`PhysicsSemantics.jsonl` and `MultiScript.jsonl` record their scenes' replays in
  `Assets/Tests/Replays/` themselves, and so does `Replays2D.jsonl` for `Sprites2D.replay` and `Physics2D.replay`, with
  `input.record` around a `play.step` of the planned input that the `Record.Sprites2D` and `Record.Physics2D` suites
  inject, and `TilemapReplay.jsonl` for `Tilemap.replay`, with `input.record` around a `play.step` of its planned input;
  M22's `UiReplay.jsonl` records its scene's replay there itself, and M22's `UiTextInputReplay.jsonl` and `UiWorld.jsonl`
  record `TextInput.replay` and `UiWorld.replay` with `test.run {record}` from their recording suites in
  `Assets/Tests/Record/`, which `Testing.Suites` keeps in the Editor mode, so re-recording one is that one call; M22's
  landing's `Combined2DUiReplay.jsonl` records `Combined2DUi.replay` with `input.record` around a `play.step` of its
  planned pointer input, and so do M23's `AnimationReplays.jsonl` for `Animation.replay` and M26's `TerrainReplays.jsonl`
  for `Terrain.replay`, each with its planned action input). M26's `Probes.jsonl` and `GoldenProbes.jsonl` run in a
  rendering editor (`Renderer` `vulkan`), whose `lighting.bake` writes their probes' bakes under `Assets/Lighting/`; its
  `PostProcessV2.jsonl` and `GoldenLut.jsonl` import a `.cube` LUT into `Assets/Luts/`, and `Terrain.jsonl` converts a
  16-bit heightmap with `terrain.importHeightmap` into `Assets/Heightfields/Terrain/` and sculpts and paints it with
  `terrain.edit`.
  `python Scripts/Scaffold.py`, run from the engine repository's root with every editor on the project closed, restores
  `Scaffold/Baseline/` and runs them all; `python Scripts/Scaffold.py --check` proves that they reproduce the project byte
  for byte. To change the content, change a scaffold and regenerate; a changed setting, scene or id re-records the replays.
- `Scaffold/Golden.jsonl` wrote the M8 golden-image scenes of §15.4 under `Assets/Scenes/Golden/` (rendered by
  `Tests/Source/Golden/`), their materials under `Assets/Materials/Golden/` and the fixtures it imports into
  `Assets/Models/Golden/` and `Assets/Textures/Golden/`. Regeneration keeps them: exactly the files
  `Scaffold/Baseline/Provenance.json` records, with its bytes, which change only through a recorded regeneration. Every
  later golden scene comes from a golden scaffold, a `Scaffolds.json` entry with `GoldenDirectories`, which writes only
  below directories of its own and is regenerated like every other scaffold: `Scaffold/GoldenRendererII.jsonl` (M18's
  recorded regeneration of `Golden.jsonl`'s M9 part) writes the Renderer II scenes under
  `Assets/Scenes/GoldenRendererII/` and their materials under `Assets/Materials/GoldenRendererII/`;
  `Scaffold/Sprites2DGolden.jsonl` (M21) writes the pixel-art scene under `Assets/Scenes/Sprites2DGolden/` and its
  texture and sprite sheet under `Assets/Textures/Sprites2DGolden/`; `Scaffold/Lights2DGolden.jsonl` (M21) writes the 2D
  lighting scene under `Assets/Scenes/Lights2DGolden/` and its normal map, sprite sheet and tilemap under
  `Assets/Textures/Lights2DGolden/`; `Scaffold/UiGolden.jsonl` (M22) writes the UI golden scene under
  `Assets/Scenes/GoldenUi/` and its sprite sheet and texture under `Assets/Textures/GoldenUi/`,
  `Scaffold/UiWidgetsGolden.jsonl` (M22) writes the widgets golden scene under `Assets/Scenes/GoldenUiWidgets/`,
  `Scaffold/UiScriptsGolden.jsonl` (M22) writes the CJK, Arabic and Hebrew golden scenes under
  `Assets/Scenes/GoldenUiScripts/` and their fonts (the Noto fixtures and a copy of Inter that falls back to them) under
  `Assets/Fonts/GoldenUiScripts/`, `Scaffold/UiWorldGolden.jsonl` (M22) writes the World canvas golden scene under
  `Assets/Scenes/GoldenUiWorld/`, and `Scaffold/Combined2DUiGolden.jsonl` (M22's landing, the combined 2D/UI test) writes
  its scene under `Assets/Scenes/Combined2DUiGolden/` and its behaviour under `Assets/Scripts/Combined2DUiGolden/`.
  M23's `GoldenPointShadows.jsonl`, `GoldenTextured.jsonl`, `GoldenClusteredLights.jsonl` and `GoldenSkinnedPose.jsonl`
  and M26's `GoldenParticles.jsonl`, `GoldenProbes.jsonl` (its bake under `Assets/Lighting/Scenes/GoldenProbes/`),
  `GoldenAutoExposure.jsonl`, `GoldenLut.jsonl` (its LUT under `Assets/Luts/GoldenLut/`), `GoldenTaa.jsonl`,
  `GoldenSsr.jsonl` and `GoldenTerrain.jsonl` (its heightfield and splat map under `Assets/Heightfields/GoldenTerrain/`)
  write their scenes under `Assets/Scenes/Golden<Name>/` and their materials, textures or models under the matching
  `Assets/<Kind>/Golden<Name>/`, each exactly its `GoldenDirectories`.
  `asset.import` reads only project paths and absolute ones, so imported files come from `Scaffold/Sources/`,
  byte-identical copies of the `Tests/Data/Assets` fixtures (`Tests/Data/Generate`), which a unit test keeps in step. The
  editor's `.bak` backups of rewritten files are not committed (`.gitignore` leaves them out).
