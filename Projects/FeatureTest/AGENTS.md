# FeatureTest

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills through automation (`docs.get`, for example `{"topic": "skills/game-building"}` once that skill exists).

- The editor writes every engine-consumed file (`FeatureTest.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- FeatureTest is the engine's coverage project (`Docs/Architecture.md` §15.5), not a game. Its scenes are produced by
  the committed batch scaffolds under `Scaffold/` (`Editor --batch`), never by hand. M8 adds the golden-image scenes of
  §15.4 under `Assets/Scenes/Golden/` (rendered by `Tests/Source/Golden/GoldenTests.cpp`): `Scaffold/Golden.jsonl`
  writes them, their materials under `Assets/Materials/Golden/` and the fixtures it imports into `Assets/Models/Golden/`
  and `Assets/Textures/Golden/`. It runs once on the project skeleton, from the engine repository's root:
  `Editor --headless --renderer none --batch Projects/FeatureTest/Scaffold/Golden.jsonl`. `asset.import` reads only
  project paths and absolute ones, so the imported files come from `Scaffold/Sources/`, byte-identical copies of the
  `Tests/Data/Assets` fixtures (`Tests/Data/Generate`), which a unit test keeps in step. The editor's `.bak` backups of
  rewritten files are not committed (`.gitignore` leaves them out). M14 completes the project with its suites, replays and the remaining scenes.
