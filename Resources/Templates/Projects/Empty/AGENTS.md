# {{Name}}

Notes for agents and people working on this game. The engine's own guide is the engine repository's `AGENTS.md`; the editor
serves its skills and generated reference through automation (`docs.get {}` lists them). Start with
`{"topic": "skills/game-building"}`, the end-to-end workflow and its definition of done; `skills/luau-gameplay` and
`skills/game-debugging` cover scripts and diagnosis, and `reference/Components`, `reference/Automation`,
`reference/Validation` and `reference/FileFormats` the engine's data.

- The editor writes every engine-consumed file (`{{Name}}.eproj`, `Assets/**` and their `.meta` files) through automation,
  and records each write in the committed `Automation/Provenance.json`; the MCP bridge appends every request to the
  committed `Automation/BuildLog.jsonl`. Do not edit those files by hand.
- `Library/` is the editor's cache, autosave and lock; it is not committed.
- Tests are Luau suites under `Assets/Tests/` listed in `Testing.Suites`, and recorded replays under `Assets/Tests/Replays/`
  listed in `Testing.Replays`; `test.run` (or `Editor --headless --project <this project> --run-tests`) runs them all.
- Describe the game here: its scenes, input actions, scripts, suites and replays.
