# FeatureTest

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills through automation (`docs.get`, for example `{"topic": "skills/game-building"}` once that skill exists).

- The editor writes every engine-consumed file (`FeatureTest.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- FeatureTest is the engine's coverage project (`Docs/Architecture.md` §15.5), not a game. Its scenes are produced by
  the committed batch scaffolds under `Scaffold/` (`Editor --batch`), never by hand. M8 adds the golden-image scenes of
  §15.4 under `Assets/Scenes/Golden/` (`Scaffold/Golden.jsonl`, rendered by `Tests/Source/Golden/GoldenTests.cpp`); M14
  completes the project with its suites, replays and the remaining scenes.
