# {{Name}}

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills through automation (`docs.get`, for example `{"topic": "skills/game-building"}` once that skill exists).

- The editor writes every engine-consumed file (`{{Name}}.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- Describe the game here: its scenes, input actions, scripts and tests.
