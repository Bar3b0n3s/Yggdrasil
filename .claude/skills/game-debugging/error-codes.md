# Error codes and fixes

A reference page of the `game-debugging` skill (`skills/game-debugging`).

## JSON-RPC errors (§13.3)

Every error response carries `error.data.errorCode` (the engine `ErrorCode` below) and `issues[]` of `{pointer, message, hint?, suggestions?}`; fix the value at each pointer.

| Code | Name | Fix |
|---|---|---|
| -32700 / -32600 | Parse error / Invalid request | Send one well-formed JSON-RPC 2.0 object per message. |
| -32601 | Method not found | Use a method `rpc.discover` lists; the hint suggests close names. |
| -32602 | Invalid params | Correct each `issues[]` pointer (unknown field, wrong type, out of range, missing required param). A `scene` member on a method that takes no scene target is refused at `/scene`, and on an op of `edit.batch` at `/ops/<i>/params/scene` (give `scene` on the batch instead). The one-script shape `{Script: {Script: ...}}` is an unknown field: give `{Script: {Scripts: [{Script: ..., Fields: {...}}]}}`. |
| -32000 | Internal | An engine bug: read `log.read` and the crash report, then report it with the request. When `error.data.errorCode` is `Io`, `Gpu` or `OutOfMemory`, the environment failed instead: see the engine `ErrorCode` table. |
| -32001 | NotFound | The entity, asset, component, file or topic does not exist: check the path or id; follow the suggestions. |
| -32002 | InvalidState | Not allowed now (no project or scene open, a test run owns the editor, another client owns lockstep, not playing): reach the needed state first. |
| -32003 | ValidationFailed | The input breaks a rule: fix each issue, then retry. |
| -32004 | Conflict | `ifRevision` no longer matches: re-read the state and retry with the current revision. |
| -32005 | Timeout | The call exceeded its deadline: bound the work (fewer ticks per `play.step`) or retry. |
| -32006 | ScriptError | A script raised during the call: read the located error and fix the script. |
| -32007 | Busy | The editor is in a long operation: retry after it ends. |
| -32008 | Unauthorized | Mutations are denied (read-only project or the deny switch): open a writable project or allow automation. |
| -32009 | Unsupported | Not available here (`dryRun` on a method without it, screenshots with `renderer: "none"`). |
| -32010 | Cancelled | The request was cancelled (client disconnect, project closed): repeat it if still needed. |

## Engine `ErrorCode` (`error.data.errorCode`, script and log messages)

| ErrorCode | Meaning | Fix |
|---|---|---|
| `InvalidArgument` | A parameter is malformed or out of range. | Correct the parameter the location names. `input.replay`'s `strictHash` and `withoutInput` each require `verify: true`, and its `continuePastCheckpoint` requires `strictHash: true`. `test.run`'s `expect` requires `record` (at `/expect`), and its expectations' ticks must be in order (at `/expect/<i>/tick`). An `Entity` value inside component data (a script's `Field.Entity()`) takes an id, a unique id prefix or a path, resolved in the scene the call targets; an ambiguous one is refused at its pointer with the candidates as suggestions. An `edit.batch` op's `name` must be an identifier of at most 64 characters, unique in the batch, and `{"$ref": "<name>.<path>"}` must name an earlier op (the hint lists them). A `$ref` path that names no member says which object it inspected (`'0.prefab' (in the result of op 0)`) and lists up to 32 of the members that object has. `prefab.apply`'s `level` must be one of the instance's levels, which the hint lists, outermost first. `scene.close` and `scene.open` take at most one of `save` and `discardChanges`, and `scene.open` and `scene.new` neither with `keepOpen` (nor `scene.open` its `reload`). During play, `target: "play"` with a scene target that names another open scene than the played one. |
| `NotFound` | A file, entity, asset, component or method does not exist. | Fix the reference; check spelling and case (paths are case-sensitive everywhere). A scene target (`/scene`) that names no open scene: `scene.list` lists them (an `id` dies with its scene). `script.eval`'s `script` (`/script`) that names no behaviour of the entity's script slots. |
| `AlreadyExists` | The name, path or id is taken. | Choose another path or open the existing item. |
| `InvalidState` | Not allowed in the current state. | Reach the state the message names (open a project, stop play, wait for a test run). "no scene open" after a launch or attach: `scene_open {path}` (the project's `StartScene`); `editor_status` shows the active scene as `session.scene`. `test.run`'s "is open with unsaved changes": `scene.save` first. `asset.delete` of a folder holding a file whose name no project path can spell (such as `nul.txt` or one ending in a dot) removes nothing: remove that file outside the editor. `prefab.unpack` of a nested instance root: unpack its outermost instance first, as the hint says (one level per call). A second `Scene.Load` or `Scene.Reload` in one frame: the first transition of a frame wins. With several open scenes: a seventeenth open scene (close one); `scene.open` or `scene.new` of a path that is already open (`scene.activate` it, or `scene.open {reload: true}`); `scene.close {save}` of a scene never saved (`scene.save {path}` first); `scene.save` to another open scene's path; `scene.activate` or `scene.close` while a test run owns the editor. |
| `Io` | The operating system refused a file operation. | Check the path, disk space and that no other process locks the file. |
| `Parse` | Malformed JSON or truncated data. | Fix the syntax at the reported line and column. |
| `Validation` | Well-formed input breaks a rule. | Fix the value at the JSON pointer; the hint says how. A recording refused because it "does not depend on its input": end it while the played state is still in the scene, record again, and confirm with `input.replay {verify: true, strictHash: true, withoutInput: true}` ("Common situations" in `skills/game-debugging`). `PREFAB_CYCLE` from `prefab.apply`: the prefabs named nest each other; nothing was written. |
| `UnsupportedVersion` | A file is newer than this build. | Use a newer engine, or restore the file from the version this engine wrote. |
| `ImportFailed` | An asset failed to import. | Fix the source file; `asset.info` shows its diagnostics. |
| `CompileFailed` | Luau source did not compile. | Fix the syntax at the reported location (`script.check`). |
| `Script` | A script raised, or a reload was refused for its errors. | Fix the script at the reported location. |
| `Gpu` | The GPU device failed or is missing, or was lost during the call (a GPU hang that the driver reset is a loss too). | Use `renderer: "none"` for logic-only work, or check the Vulkan driver. A lost device is recovered at the next frame boundary, play going on as if nothing happened: wait for the `GpuDeviceLost` event (`events.read {types: ["GpuDeviceLost"]}`), then repeat the request. |
| `Timeout` | An operation exceeded its budget. | Bound the work or raise the budget the message names. |
| `PermissionDenied` | A write to a read-only project or mount, or, with mutations denied, a request that would discard an open scene's unsaved changes (`scene.close {discardChanges}`, `scene.open {discardChanges}`, `session.shutdown {force}`). | Open the project writable; never write under `engine://`; save the scene, or allow mutations, before discarding. |
| `Unsupported` | Not available in this host or configuration. | Use a host that supports it (an editor with a renderer, a non-Dist build). |
| `Conflict` | An optimistic-concurrency mismatch. | Re-read and retry with the current revision. |
| `Cancelled` | The operation was cancelled. | Repeat it if it is still wanted. |
| `OutOfMemory` | The request (a screenshot, a resize, a debug view) ran out of GPU memory again after the engine freed what it could rebuild and retried once; only it failed, and the editor goes on (JSON-RPC Internal). | Ask for less (a smaller `width` and `height`, no `debugView`), or repeat it later; a debug view that ran out of memory stays on its previous mode (`viewport.setOptions {}` reads it). |

## Script error kinds (`script.errors` rows, §11.7)

| Kind | When | Fix |
|---|---|---|
| `compile` | The source or a required module does not compile, a require does not resolve, or the module top fails at load time (an engine call at the module top, an error, the 250 ms load budget). Also a reload refused for these. | Fix the reported location; keep module tops free of engine calls (do that work in callbacks). |
| `type` | A reload refused because the new source has type errors while `Scripting.BlockPlayOnTypeErrors` is set, or in a test run (callback `Reload`). The running code is kept. | Fix the type error at the reported location (`script.check`), then save again to reload. |
| `runtime` | A callback, task or test case raised. The instance is disabled. | Fix the line in the traceback; guard optional values (`if self.Goal then`). |
| `timeout` | A callback ran past `Scripting.CallbackBudgetMs`. | Remove the unbounded loop, or spread the work over ticks with a task. |
| `memory` | The VM passed `Scripting.MemoryLimitMB`; a second breach stops the session. | Stop allocating per tick (pool tables and entities), or raise the limit deliberately. |

## MCP bridge errors

| Error | Fix |
|---|---|
| `EditorCrashed {exitCode, lastLogLines, crashReportPath, autosaveAvailable}` | Read the crash report and the last log lines; relaunch with `editor_launch {recover: true}` when an autosave exists. |
| `EditorAlreadyOpen {pid}` | An editor without automation holds the project: enable "Allow AI automation" in its Automation panel, then `editor_attach`. |
| `LockUnavailable` | A gate (`CI.py`) runs, or, for `renderer: "vulkan"`, another GPU run holds the machine-wide GPU lock (the message names its pid, worktree and command; `python Scripts/Locks.py` lists the holders): call `editor_launch` again once it has ended, or launch with `renderer: "none"` when no screenshot or smoke test is needed and no gate runs. |
| `NotConnected` | Call `editor_launch` or `editor_attach` first; unconnected, `editor_status` lists the editors running with automation (pid, project, renderer) that `editor_attach {pid}` reaches. |
| `ConnectionClosed` | The editor closed the connection: check `editor_status`, then relaunch or reattach. |
| `Timeout` | The call exceeded the method's `timeoutSeconds`: retry with less work. |
| `UnknownTool` | Use a tool the bridge lists; reach any other method through `engine_call {method, params}`. |
| `InvalidParams` / `InvalidState` | Fix the bridge tool's arguments, or `editor_shutdown` before launching another editor. |
| A missing editor binary | Build it: `python Scripts/Build.py --config Release --project Editor`. |
