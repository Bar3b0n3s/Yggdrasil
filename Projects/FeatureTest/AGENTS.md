# FeatureTest

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done; `skills/luau-gameplay` and
`skills/game-debugging` cover scripts and diagnosis, and `reference/Components`, `reference/Automation`,
`reference/Validation` and `reference/FileFormats` the engine's data.

- The editor writes every engine-consumed file (`FeatureTest.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`. Do not edit those files by hand. FeatureTest has
  no `Automation/BuildLog.jsonl`: its content comes from batch scaffolds run with `Editor --batch` (below), never through
  the MCP bridge, which is what appends that log. `Automation/Provenance.json` has an entry for every file the scaffolds
  write, but none for the 24 files of the M9 golden content (the Renderer II scenes under `Assets/Scenes/Golden/` and
  the materials under `Assets/Materials/Golden/RendererII/`, each with its `.meta`): they are the output of
  `Scaffold/Golden.jsonl`'s last 30 requests replayed on their own by a unit test ("RendererIIGolden: scaffold reproduces
  the pinned scenes materials and metadata", which pins them byte for byte), copied in rather than written by an editor
  run of this project (`Docs/Decisions/0022-m14-contract.md` "Integration").
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- FeatureTest is the engine's coverage project (`Docs/Architecture.md` §15.5 and §15.6), not a game. It sets
  `Testing.CoverageGates`, so an unfiltered `Editor --headless --project Projects/FeatureTest --run-tests` (or `test.run`)
  evaluates the six per-mode coverage gates and fails on any uncovered component, field, enum value, script API, callback,
  proxy field or source format (`python Scripts/Test.py --suite feature`).
- Its content is produced by the committed batch scaffolds under `Scaffold/` (`Editor --batch`), never by hand.
  `Scaffold/Scaffolds.json` lists them in run order, each with its id seed (`Editor --id-seed`) and renderer:
  `Settings.jsonl` writes the project settings, the stream scaffolds write the scenes, scripts, suites and test assets
  under `Assets/Scenes/`, `Assets/Scripts/`, `Assets/Prefabs/`, `Assets/Formats/` and `Assets/Tests/<Area>/`,
  `Suites.jsonl` writes `Testing.Suites` and `Testing.Replays`, and `Replays.jsonl` records `Input.replay` and
  `Determinism.replay` last (`PhysicsSemantics.jsonl` and `MultiScript.jsonl` record their scenes' replays in
  `Assets/Tests/Replays/` themselves).
  `python Scripts/Scaffold.py`, run from the engine repository's root with every editor on the project closed, restores
  `Scaffold/Baseline/` and runs them all; `python Scripts/Scaffold.py --check` proves that they reproduce the project byte
  for byte. To change the content, change a scaffold and regenerate; a changed setting, scene or id re-records the replays.
- `Scaffold/Golden.jsonl` writes the golden-image scenes of §15.4 under `Assets/Scenes/Golden/` (rendered by
  `Tests/Source/Golden/GoldenTests.cpp`), their materials under `Assets/Materials/Golden/` and the fixtures it imports into
  `Assets/Models/Golden/` and `Assets/Textures/Golden/`; it and its content stay untouched (the baseline holds them).
  `asset.import` reads only project paths and absolute ones, so imported files come from `Scaffold/Sources/`,
  byte-identical copies of the `Tests/Data/Assets` fixtures (`Tests/Data/Generate`), which a unit test keeps in step. The
  editor's `.bak` backups of rewritten files are not committed (`.gitignore` leaves them out).
