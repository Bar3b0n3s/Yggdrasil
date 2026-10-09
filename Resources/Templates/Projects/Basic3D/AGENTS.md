# {{Name}}

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills through automation (`docs.get`, for example `{"topic": "skills/game-building"}` once that skill exists).

- The editor writes every engine-consumed file (`{{Name}}.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- Describe the game here: its scenes, input actions, scripts and tests.

The Basic3D project starts in `Assets/Scenes/Main.scene`, included in `Export.BuildScenes`. It contains a primary
camera and audio listener, directional sunlight, the built-in Studio environment, post-processing, and a static
box-shaped ground. Its mesh, material and environment are engine built-ins; no external asset download or script is
needed. Add gameplay through the editor's automation methods and save the scene before running or exporting it.
