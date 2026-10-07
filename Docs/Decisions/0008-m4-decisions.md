# 0008 — M4 contract decisions

- **Status:** proposed by the M4 contract task and completed by the M4 integration task (decisions 29 to 35), which also resolved the streams' requests to the contract owner, and by the integration review (decision 36). The docs owner applies the amendments listed at the end.
- **Date:** 2026-10-06
- **Context:** The M4 contract task (Roadmap rule 3) froze the public interface of every M4 deliverable: the automation protocol (`Engine/Source/Engine/Automation/Protocol/`), EditorCore (editor state, commands, projects, validator, provenance, the automation server and every M4 method), the editor's command line, the Python client, the MCP bridge and both Python test suites. Writing complete headers exposed places where the Architecture is silent, sketches an interface that cannot be built as written, or conflicts with an earlier ADR. `AGENTS.md` ("Deviations") requires a record of each.

## Decisions

### 1. Files beyond the Roadmap M4 list

Each one is needed by a frozen interface or an acceptance test:

- **Protocol** (`Engine/Source/Engine/Automation/Protocol/`): `JsonRpc` (messages, error codes, `ProtocolVersion`, `ClientId`), `Framing`, `Watchdog`, `Handshake` (token and hello checks), `SessionFile`, `ProtocolServer` (TCP transport and I/O thread), `MethodContext`, `PendingOperation` (with `MethodResult`), `MethodRegistry`, `Dispatcher` (the §4.6 item 5 boundary file, already allowlisted), `MetaBuilder`, `ResultOffload`, `JsonReference` (`$ref`).
- **EditorCore:** `EditorContext.h` also declares `EditorTransaction` and `EditorDryRunScope`, the two helpers that only serve it. `Commands/{Command,CommandHistory,SceneEdit,SceneEditCommand,CompositeCommand,ProjectSettingsCommand}`, `Project/{ProjectManager,ProjectValidator}` (`ProjectManager.h` declares `LoadedProject`, the open project it returns), `EditorCommandLine`, and in `Automation/`: `AutomationServer`, `EditorMethodContext`, `AutomationTypes` (shared reflected types), `RegisterMethods`, one `<Domain>Methods.h|.cpp` per domain (session, rpc, project, scene, entity, component, edit, observe, debug), `ProvenanceRecorder`, `TranscriptLog` (the editor's transcript lines; its own file pair, CodeStyle §4.1), `BatchRunner` (`--batch` and `--upgrade`) and `JsonPatchDiff` (RFC 6902 for `scene.diff`).
- **Platform:** `Socket.h` gains `Socket::SendAvailable` and a `WaitAny` overload that reports writable connections (decision 27).
- **Resources:** `Resources/Templates/Projects/Empty/{.gitignore,.luaurc,AGENTS.md}` (§2.1 `Templates/{Projects,Scripts}/`).
- **Tests support:** `Support/EditorTestFixture`, `Support/AutomationTestClient` (with `AutomationFixture`), `Support/ProtocolTestTypes`; Python `Tests/Automation/harness.py`.
- **Added by the streams** (accepted at integration, decision 30): `Editor/Source/EditorCore/Automation/Private/MethodSupport.h|.cpp` (what the domain method files share: located param errors, load-diagnostic logging, the project and scene summaries results carry, and the scene file operations of scene.*, project.* and session.shutdown), `Scripts/Lib/unittest_runner.py` (runs the Python suites with JUnit output for `Test.py`), `Tests/Automation/fake_editor.py` (a stand-in server, lock holder and session-file writer for client tests that need no editor), `Tests/Automation/test_client.py` and `Tools/MCP/tests/test_bridge.py`.
- **Added by the integration review** (decision 36): `Editor/Source/EditorCore/Private/EditorFileError.h|.cpp` (decision 5's one conversion of operating-system access failures, shared by `EditorContext` and the method files) and `Tests/Source/Support/WaitUntil.h` (decision 15's bounded wait).

### 2. The type registry joins EngineContext

§4.1 puts `TypeRegistry` in `EngineContext`, and ADR 0005 decision 12 appends later services instead of pre-declaring them. M4 is the first consumer, so the contract appended it, and implemented it because every stream depends on it:

- `EngineContext` constructs it in the infallible Services step: `RegisterBuiltinComponents`, `RegisterProjectSettingsTypes`, then `EngineContextSpecification::RegisterTypes` (a function pointer, null by default), then `Freeze`. `GetTypeRegistry()` returns it const.
- `ApplicationSpecification::RegisterTypes` passes the hook through. The editor's factory sets it to `RegisterEditorMethodTypes`.
- Tests: "EngineContext: the type registry holds the built-in components and the project settings types, frozen" and "EngineContext: RegisterTypes adds the application's types before the registry is frozen".

**One registry, not one per protocol.** Automation params embed component values and results embed project settings, and full schemas need component `$defs` (§13.4), so the automation structs live in the same registry as the components, as §5.4 and `BuiltinComponents.h` already say ("the caller adds its own types (project settings, automation structs)").

### 3. The frame's safe point

ADR 0005 decision 12 recorded that the M4 contract extends `FrameLoop.h` for automation. `IFrameLoopClient::OnFrameSafePoint()` (default: nothing) runs after the `MainThreadQueue` drain and before the steps (§4.2 step 3), with the `FramePhase` breadcrumb "SafePoint". `Application` forwards it to the protected `OnSafePoint()`, where `EditorApp` pumps its server. Implemented by the contract, with the test "FrameLoop: the safe point runs once per frame after the queue drain and before the steps". Lockstep, time scale and throttle control remain M7's planned extension.

### 4. Headless editors keep the frame throttle

A headless editor pumps automation once per frame at `FixedHz` (60 Hz), which bounds request latency at about 16 ms. That is accepted, because §4.2 forbids unthrottled headless play. Batch runs (`--batch`, `--upgrade`) set `ThrottleHeadless` off, as §4.2 lists them among the unthrottled modes.

### 5. Wire conventions

- **Request metadata:** `params._meta.transcriptLine`, as in MCP's `params._meta`. JSON-RPC request objects get no extra members.
- **Response metadata:** `result._meta` on success, `error.data._meta` on errors. A dry run's result also carries `"dryRun": true`.
- **Error object:** the `message` is JSON-RPC's standard text for the standard codes and the engine message otherwise. `data` holds:
  - `errorCode`, the `ErrorCode` name;
  - `detail`, the engine message;
  - `hint`, `contexts` and `location`, when set;
  - `issues`, always present, each `{pointer, message, hint?, suggestions?}`;
  - the method's extra members (`failedOp`, `currentRevision`);
  - `_meta`.
- **Code mapping** (§13.3 lists the codes but not the mapping):
  - InvalidArgument → InvalidParams;
  - Validation, Parse, UnsupportedVersion and ImportFailed → ValidationFailed (−32003);
  - AlreadyExists and Conflict → Conflict;
  - Script and CompileFailed → ScriptError;
  - PermissionDenied → Unauthorized (−32008), which covers bad tokens and mutations refused by read-only editors. PermissionDenied is reserved for these refusals of the protocol and the editor: an operating-system access failure (EACCES, EPERM, a Windows sharing violation), which `FileSystem` reports as PermissionDenied, is converted to Io by the editor's file paths (`EditorContext::WriteProjectFile` and its reads, scene loads, `docs.get`, `project.upgrade`), so a locked file is never reported as an authorization failure. One function does it, `Utils::ToEditorFileError` (`EditorCore/Private/EditorFileError.h`), with one wording: "<message> (the operating system denied access)";
  - Unsupported → Unsupported;
  - Unknown, Io and Gpu → Internal.

  Param parsing failures are always InvalidParams.
- **Not accepted:** batch arrays, positional params, and `null` ids (a notification omits `id`). Notifications run but get no response.
- **Engine version:** `EngineVersionString = "0.1.0"`, a constant until the export milestone defines how builds are versioned (replays already record `EngineVersion`).

### 6. The I/O thread authenticates

The token and version checks of `session.hello` run on the I/O thread, so unauthenticated traffic never reaches the main thread and three failures close the connection even while the main thread is busy.

- **Counted as failures:** any request before a successful hello ("session.hello must be the first request"), a bad token, malformed hello params.
- **One strictness rule:** `ParseHelloRequest` rejects unknown members exactly as the handler's param struct does, so a hello the I/O thread accepts never fails on the main thread. session.hello's params never gain members within a major version; clients send a newer minor version's optional members elsewhere only after reading the server's version from the hello result (`JsonRpc.h`).
- **Closes the connection at once:** an incompatible major version.
- **A good hello:** queues `Connected` and then the hello itself, which the `session.hello` handler answers with the session report. While the watchdog reports a stall, it is answered Busy at once instead and the connection stays unauthenticated, without a failure counted, so a client never hangs on a frozen editor (§13.2) and retries the hello.
- **Client slots:** `MaxClients` (4) counts authenticated connections only; a good hello beyond it is answered InvalidState and closed. Connections still in the handshake have their own cap (`MaxPendingHandshakes`, 8, beyond which a connection is closed at accept) and a deadline (`HandshakeTimeout`, 10 s), so idle sockets from any local process can neither lock the bridge out nor pile up. The I/O thread's clock is injectable (`ProtocolServerSpecification::TimeSource`) so these are tested without waiting.
- **Frames before authentication:** a connection may send frames of at most `MaxHandshakePayloadBytes` (64 KB; a hello is under 1 KB) until its hello succeeds, then of up to `MaxFramePayloadBytes` (`FrameDecoder::SetMaxPayloadBytes`). A larger Content-Length closes the connection at the header, before any payload is buffered or parsed, so a process without the token cannot make the editor hold or parse large frames.
- **Rejections are rate-limited:** connections closed before they authenticated (refused at accept, invalid frames, failed handshakes, an incompatible version, an overflowing queue) are logged at Warn at most once per `ProtocolServer::RejectionReportInterval` (10 s): the first at once, later ones as a count with the latest reason, so a local process that connects in a loop cannot flood the log and every agent's `_meta.diagnostics`.

### 7. Host contexts without RTTI

`MethodRegistry` lives in the protocol layer and cannot name `EditorContext` (§3).

- **Base class:** `MethodContext`. Hosts derive their own context (EditorCore's `EditorMethodContext`, from M7 the Runtime's), and each passes `TypeKeyOf<itself>()` to the base.
- **Typed handlers:** `Result<R>(Host&, const P&)` for `Add`, and `Result<Scope<PendingOperation>>(Host&, const P&)` for `AddPending`. They can use `ENGINE_TRY`, which §13.2's untyped `std::variant<Json, Error, Scope<PendingOperation>>` (kept as `MethodResult`, the erased form) cannot return. `Host` may be a host's own context or a base several hosts share (decision 26).
- **The cast:** the registry casts to the handler's host type only after `MethodContext::IsHostType` confirms it: every context answers for its most-derived key and `MethodContext`, and a shared intermediate base overrides `IsHostType` to add its own key and calls its base. First-party code uses neither `dynamic_cast` nor `typeid`.
- **What the protocol needs from its host:** `IMethodHost`. The one-time admission of a request is separate from the per-invocation setup, so a pending operation is admitted once and then polled and cancelled without being checked again:
  - `CheckAvailability(descriptor)`, right after the method is found and before its params are read (the launcher state, from M7 the play state), so an unavailable method is reported as such whatever its params;
  - `CreateContext`;
  - `AdmitRequest`, once: read-only and deny-mutations refusals, the `ifRevision` check, and opening a dry run's sandbox;
  - `EnterInvocation` and `LeaveInvocation`, infallible, around the handler, every `Poll` and the `Cancel`: write attribution and command origin. `Cancel` therefore always runs and cannot be refused, which lockstep and recordings rely on (§13.2 "Disconnect");
  - `FinishRequest`, once at the end (answered, refused or cancelled): closes the sandbox before `_meta` is built;
  - `GetMetaState`, `GetOffloadServerTag` and `WriteOffloadedResult`.

  `Dispatcher` keeps the per-client queues and pending operations in the protocol layer, so the Runtime reuses them in M7. A client can be added without offloading (`Dispatcher::AddClient`, `AutomationServer::ConnectInProcess`), which `BatchRunner` uses so its `$ref` substitution reads full results.
- **Exceptions:** an exception escaping a handler asserts in Debug builds and becomes an Internal response in Release (§4.6 "asserts in Debug").

### 8. Param-struct conventions

They are listed in `MethodRegistry.h` and binding for every method. The points that go beyond the Architecture:

- **JSON keys:** registered field names are camelCase JSON keys (§13.4), where `FieldInfo.h` describes field names as PascalCase, the rule for authored files. The registered name is the member name with a lower-cased first letter unless a comment in the header names another key, so the headers are the wire contract.
- **Required params:** the registry has no required-field flag, so `MethodSpecification::RequiredParams` lists them. They are checked before parsing and become `required` in the schemas.
- **Absent params:** "absent" and "default" are told apart with `MethodContext::HasParam`, so `entity.update {name?}` needs no optional `FieldType`.
- **Reserved params:** `dryRun`, `ifRevision` and `_meta` are never declared. The registry takes them out first, so a method without `supportsDryRun` can answer Unsupported instead of "unknown field". Every method accepts `ifRevision` (§13.4 defines it for calls in general): a call that changes something only when asked (`project.validate {fix}`, `scene.open {save}`, `session.shutdown {save}`) is guarded like a mutation, and for a read it is a precondition. Every params schema lists it, except the `session.hello` handshake's, which the I/O thread reads strictly.
- **Enum spelling:** enums are rewritten to their canonical names before strict parsing, embedded component data included. Readers of authored files stay case-sensitive (§6).
- **64-bit counters:** the registry has no 64-bit integer `FieldType`, so revisions (`EditorContext::GetRevision`, decision 28), undo indexes and ticks are `uint32_t` in param and result structs and saturate (`ToAutomationCounter`). `ifRevision`, `transcriptLine` and `_meta` are read or written outside the registry and keep 64 bits; both forms agree below 2^32.
- **Cursors:** every `cursor`/`nextCursor` is a string (convention 9). Log and event cursors are the decimal 64-bit sequence numbers of their logs, so they never saturate, and also accept `"end"`, which reads nothing and returns the cursor of the next entry (the "now" cursor a caller takes before an action). `_meta.diagnostics.logCursor` is such a string.
- **Batch ops:** `MethodSpecification::AllowedInBatch` marks the methods `edit.batch` may run as ops: pure reads and methods whose every effect goes through `EditorContext::Execute` (entity.*, project.setSettings, project.validate's fixes). Methods that replace the open scene, write files outside a command, record provenance outside a command or end the session (scene.open, scene.new, scene.save, project.save, project.upgrade, session.shutdown), edit.select, edit.batch itself and every pending method are not, so §13.4's batch is really atomic. `MethodRegistry::InvokeNested` rejects other methods, and ops whose params carry `dryRun`, `ifRevision` or `_meta` (located at `/ops/<k>/params/<member>`), before anything runs; every op runs with the batch's options.
- **Polymorphic params:** `fix: true | [...]` and `components: [...] | "all"` are free-form `VariantValue` members that the handlers validate.
- **Component maps:** they use `ResolveComponentValue`, so `/components/RigidBody/Mas` is an unknown-field issue with "did you mean 'Mass'?" (§13.3's example).
- **No params:** methods that take none share `NoParams`.
- **Name collisions:** a name an engine type already takes gets "Method" before its suffix (`LogReadMethodResult`).

### 9. The method catalogues

- `Editor --dump-reference <dir>` writes `<dir>/Methods.json` (`{"Format": "MethodCatalog", ...}`: every method except test hooks, with full schemas) and `<dir>/catalog.json`, the MCP tool catalogue.
- The catalogue has the shape `{"Format": "McpCatalog", "Version": 1, "ProtocolVersion", "Tools": [{name, method, description, inputSchema, mutates, supportsDryRun, timeoutSeconds}]}`.
- The C++ registry produces the compact schemas (§13.8), so the bridge uses `inputSchema` verbatim.
- `Tools/MCP/catalog.json` is that file, committed. The contract commits an empty but valid catalogue. Stream D regenerates it at integration, when the methods exist, and the tests "EditorApp: --dump-reference writes the method catalogue and the MCP catalogue" and `test_rpc_discover_matches_the_dumped_catalogue` keep it current until M14's `GenerateDocs.py --check` takes over.

### 10. The provenance format

The frozen format is in `ProvenanceRecorder.h`: `{"Format": "Provenance", "Version": 1, "Entries": [{"Path", "XXH64", "Method", "RequestId", "Client", "TranscriptLine"}]}`.

- Keys are PascalCase like every file the canonical writer writes (§6), where §13.4 lists them in lowercase.
- `XXH64` is 16 hex digits (never a JSON number, as for UUIDs, §4.8).
- `RequestId` is the JSON-RPC id as sent.
- `RequestId` and `TranscriptLine` are `null` for `ui` and `cli` writes and for requests without a transcript line.
- Every project write goes through `EditorContext::WriteProjectFile`. It refuses writes in read-only editors, sends them to the overlay during dry runs, and otherwise records the `.eproj` and `Assets/**` with the current attribution, then saves the provenance file.

### 11. The transcript line format

Shared by `engine_mcp/transcript.py` and `TranscriptLog`, camelCase like the protocol, appended and never rewritten:

```
{"type": "request", "time": "<UTC ISO 8601>", "client": "<name>", "id": <id>, "method": "<method>", "params": {...}}
{"type": "response", "time": "...", "client": "<name>", "id": <id>, "requestLine": <n>, "ok": true|false, "summary": "...", "error"?: {"code", "errorCode", "detail"}}
```

- Line numbers are 1-based.
- `session.hello` is never written, because it carries the token.
- The editor writes its own lines only for `--upgrade`, with client "cli" (§13.12). A line is added by an atomic rewrite of the file, because the VFS has no append.

### 12. Dry runs use a scene copy

§13.4 says "scene mutations execute then undo inside a sandboxed history", but undo increments the revision and `test_dry_run_leaves_revision_unchanged` requires an unchanged revision. `EditorDryRunScope` therefore swaps in, for the call:

- a serializer copy of the open scene (the one scene-copy path), with a sandbox history and a copy of the id generator;
- an `OverlayMount` for `project://`, entered only after `JobSystem::WaitIdle`, per ADR 0003 decision 13.

It also suppresses events and provenance, and adjusts the revision base so `EditorContext::GetRevision` reports the real revision plus the sandbox's mutations. The destructor restores everything. The ids and revisions a dry run reports are the ones the real call produces next.

A read-only editor may dry-run, since a dry run writes nothing.

### 13. Method semantics the Architecture leaves open

- `project.create` also opens the project, which it locks. `project.open {recover}` arrives with autosave (M10).
- **`project.setSettings` writes through:** `ProjectSettingsCommand` writes the `.eproj` on Execute and on Undo, so the file always equals the running settings and every write is attributed. `project.save` writes the dirty open scene (dirty native assets join in M6). Settings commands do not make the scene dirty (`Command::ChangesScene`).
- **`project.upgrade`** is a file-format operation, not an undoable command. It needs a clean open scene and reloads the scene when its file changed. Its dry run reports the changed files through the overlay.
- **`scene.new`** writes its file at once (so `BUILD_START_SCENE_MISSING` clears, §13.11 step 3) and takes `save?`/`discardChanges?` like `scene.open`, which §13.5 does not list. For both, a dirty open scene needs exactly one of the two (neither or both is InvalidState, as §13.5 says); both together for a clean scene is InvalidParams.
- **`entity.create`** requires `name` (§13.5); there is no default name.
- **Entity events:** every scene edit appends to the event log what it did (§4.9), in UUID order: `EntityCreated` (with the name), `EntityDestroyed`, and one `ComponentChanged` per component that was added, removed or edited (`SceneEditCommand::AppendChangeEvents`). `SceneEdit::Commit` appends a new edit's events, and undo and redo append those of each replay; a dry run appends none. A change of only the name, the activity, the tags or the parent has no event type in §4.9 and appends nothing.
- **`scene.open`** of the open scene's own path needs `reload`.
- **The undo history belongs to the open scene** and is cleared when another scene opens (v1 edits one scene at a time, §12.1).
- **`scene.diff {against: "revision"}`** accepts only revisions the history still holds. It reconstructs older entity states from the `SceneEditCommand` changes (`SceneEditCommand::ApplyChanges` on a scratch copy).
- **`scene.tree {format: "json"}`** returns a flat list in canonical order, with depth and parent, instead of a nested tree. That keeps the reflected result non-recursive.
- **`entity.get`** returns every component when `components` is absent.
- **`edit.select`** is editor state, not a command.
- **`debug.pend {frames}`** is the test-hook pending operation of `test_disconnect_cancels_pending_operations`. It logs "Started debug.pend of client '<name>'" when it becomes pending, so the test waits for it before it disconnects (requests of different clients have no order). The `debug.*` hooks are not available in the launcher state: §12.1's list has no exception for them.
- **Transactions nest by joining:** an `EditorTransaction` opened while another is open joins the outermost one, so `ProjectValidator::Fix` run by an op `project.validate {fix}` becomes part of the batch's single undo step instead of asserting.

### 14. Mutates, read-only and the launcher state

- **`Mutates` is precise:** the method changes project files or the open scene's content. Methods that only change what the editor shows (`scene.open`, `edit.select`) or write only when asked (`session.shutdown {save}`, `project.validate {fix}`) are not flagged. `EditorContext::Execute` and `WriteProjectFile` refuse their writes in read-only editors with PermissionDenied. Read-only editors can therefore open scenes and validate.
- **The editor's refusals, in order:**
  1. `AutomationServer::CheckAvailability`: outside `AvailableInLauncher`, any method while no project is open (InvalidState "no project open..."), before the params are read;
  2. `AutomationServer::AdmitRequest`: a `Mutates` method in a read-only editor that is not a dry run (PermissionDenied);
  3. `AdmitRequest`: a stale `ifRevision` (Conflict with `data.currentRevision`), checked once when the request starts.
- **`--headless` listens** without `--automation`, as §13.2 lists it, except in one-shot runs (`--batch`, `--upgrade`, `--dump-reference`): `EditorLaunchOptions::ListensForAutomation(headless)` is `Automation || (headless && !IsOneShot())`, so no MCP `editor_launch` can attach to a batch or upgrade run and interleave mutations with it. A one-shot run that should listen says `--automation` (allowed with `--batch`). So only a windowed editor without `--automation` holds the lock without listening, which is the `EditorAlreadyOpen` case of §13.8.
- **The editor's command line** (`EditorCommandLine.h`, parsed in EditorCore so the rules are unit-tested):
  - `--renderer vulkan|none` is accepted before M5 (nothing renders either way in M4).
  - `--read-only` and `--upgrade` need `--project`.
  - `--batch`, `--upgrade` and `--dump-reference` exclude each other.
  - `--automation-test-hooks` needs `--automation` or `--batch`.

### 15. The watchdog test waits past the threshold; bounded waits are not timing

The watchdog's 5 s threshold is wall-clock time. A request sent before the threshold is queued and answered after the stall, not answered Busy, so `test_busy_watchdog_reports_phase` waits past 5 s after starting a 10 s `debug.stall` before it sends its request. This is the Python suite's one wall-clock exception, like ADR 0005 decision 13's minimized-window test. The C++ watchdog tests take time as a parameter. The C++ Busy test starts a server with an hour-old heartbeat and needs no waiting.

**Bounded waits.** Tests of concurrent code wait for what another thread or process does: an I/O thread closing a socket, a server queueing a request, a process taking a lock. Such a wait is bounded by a generous deadline that only bounds a failure (`Test::WaitUntil` in C++, 30 s; the Python polls of another process's state; client timeouts in tests whose peer never answers). It is not a wall-clock exception, because no passing outcome depends on how long anything takes: the deadline is never asserted on, and a fast or slow machine passes the same way. A spin count never bounds a wait: how long it waits depends on CPU speed and scheduler load, so a loaded runner can end it before the other thread has run. `AGENTS.md` "Tests", `CodeStyle.md` §14 and `ReviewChecklist.md` §8 say so.

### 16. Python contract markers are a mechanical gate

ADR 0004 gates C++ stubs and skips. The M4 contract has Python stubs and skipped Python tests, so `Lint.py`'s contract step gained two rules with path scopes in `ModuleRules.json`, which `--allow-contract-stubs` lifts like the C++ ones:

- **`contract-stub`:** in `Contract.PythonStubFiles` (`Tools/**`, `Tests/Automation/**`), a stub raises `NotImplementedError("contract stub ...")`, with a plain string or an f-string whose literal start is the marker.
- **`test-skip`:** in `Contract.PythonTestFiles` (`Tests/Automation/**`, `Tools/MCP/tests/**`), every unittest or pytest skip marker is a finding:
  - the `skip`, `skipIf`, `skipUnless` and `expectedFailure` decorators;
  - `pytest.mark.skip`, `skipif` and `xfail`;
  - `self.skipTest(...)`;
  - raising `SkipTest`;
  - the imperative `pytest.skip(...)` and `pytest.importorskip(...)` calls.

  Python tests have no permanent skips: a missing editor build or virtual environment is a failure.

The rules are AST-based, so comments and strings never count. The self-test fixture is `Tests/Data/Lint/PythonContract`. Stream D's `Test.py` automation suite must also fail on skipped tests, the runtime half that matches ADR 0004 section 3.

### 17. The M4 validator

- **Codes:** `ProjectValidator::GetCodes()` lists what M4 detects:
  - the `SCENE_*` codes of §13.7;
  - `ENTITY_DUPLICATE_ID` and `ENTITY_DANGLING_REFERENCE`;
  - `COMPONENT_FIELD_OUT_OF_RANGE`, `COMPONENT_MISSING_REQUIREMENT` and `COMPONENT_CONFLICT`;
  - `BUILD_START_SCENE_MISSING` and `BUILD_SCENE_MISSING`;
  - `ASSET_IMPORT_FAILED` for a scene file that cannot be loaded at all. Scenes are assets, and M6 reports the same code for every asset type.

  The other codes arrive with their subsystems.
- **Load diagnostics** map through `MapLoadCode`, which implements ADR 0006 decision 35. Three validator codes reuse their load-code constants (`Scene/LoadReport.h`).
- **Fixes** apply to the open scene and the settings, as one undoable command (§13.7). The structural codes exist only in files, because every write path keeps the open scene valid. They are reported without a fix, with the hint "scene.open {repair: true}, then scene.save". Fixing a file that is not open needs a file-edit command (M6).
- **Ids:** a diagnostic id is `<CODE>-<12 hex digits of XXH64(code|file|entity|component|field|subject)>`, unique within a report (asserted). The subject tells apart problems of one code at one location without depending on positions a fix elsewhere shifts: the missing scene path for `BUILD_SCENE_MISSING`, the dangling UUID or map key for `ENTITY_DANGLING_REFERENCE`; empty when the location is unique.

### 18. Commands

`Command.h` follows §12.3, with these additions:

- `GetMergeKey()` makes `MergeWith`'s downcast safe without RTTI: the history asks only when the origins and non-empty keys are equal, and equal keys imply equal types.
- `ChangesScene()` keeps settings commands from dirtying the scene.
- `GetMemorySize()` serves the 256 MB bound.
- `Origin` is an accessor pair, because a class with virtual functions has no public data members (CodeStyle §4.5).

Further rules:

- **`Undo` returns `Status`:** like `Execute` it is atomic, restoring the prior state or failing with the command still applied. An in-memory snapshot restore (`SceneEditCommand`) cannot fail, so a failure there is a bug and asserts; external failures are returned, because `ProjectSettingsCommand` writes the `.eproj` on Undo (decision 13) and M6's asset commands (§12.3: `AssetEditCommand`, `AssetMoveCommand`, `AssetDeleteCommand`) do file I/O on Undo. An assert must never be the only guard against a full disk or a locked file. `CommandHistory::Undo` returns `Result<size_t>` and stops at the first failing entry, which stays applied (mirroring Redo); `edit.undo` reports it with `data.undone`. `CompositeCommand::Undo` executes again the children it already undid when one fails, and `EditorTransaction::Rollback` records the commands still applied as one "(partially rolled back)" step, so the history always matches the scene.
- **Commands built applied:** `SceneEditCommand` is built in its applied state, so its first `Execute` changes nothing (`SceneEdit::Commit`).
- **Transactions:** `CompositeCommand` holds unexecuted children (Execute runs them) or executed ones, as an `EditorTransaction` collects them.
- **History bounds:** `EditorContextSpecification::HistoryLimits` makes the bounds configurable, so the 10,000-operation property test can run in one history.

### 19. Projects

- **Templates** are read from a native directory (`Resources/Templates/Projects/<Template>/`) given by the editor. `engine://` is not mounted before the asset milestones.
- **Return value:** `ProjectManager::CreateProject` returns the files to record in provenance.
- **Recent list:** `user://Editor.json` (`{"Format": "EditorPreferences", "Version": 1, "RecentProjects": [...]}`, at most 10; other members preserved for M10's preferences).
- **Read-only:** a read-only open takes no lock and writes nothing under the project, `Library/` included. Its cache is the private directory the editor names (`<UserData>/ReadOnlyCache/<pid>`), cleared at open when a read-only editor that did not exit cleanly left it behind (a reused process id must not inherit another project's cache) and removed when the project closes (`~LoadedProject`).
- **Entity ids:** `EditorContextSpecification::IdGeneratorState` is optional. Absent, `EditorContext::Create` seeds the generator from the OS CSPRNG, so every session's ids are unique (§4.8); tests pass a fixed state. A fixed default on the production type would repeat the id sequence in every session.

### 20. Session files

- **Keys:** camelCase, as §13.2 lists them.
- **Permissions:** on POSIX the directory is created 0700 and the file 0600, set before the atomic rename makes the file visible. Windows relies on the per-user `%LOCALAPPDATA%` ACL.
- **Rewrites:** the server rewrites the file when the open project changes, so a bridge finds an editor opened in the launcher state. A failed rewrite (on Windows the replace fails while a client such as `engine_client.list_sessions` has the file open) is tried again by later pumps, at most once per `AutomationServerSpecification::SessionFileRetryInterval` (1 s), and reported once per project; the project is remembered only after a write succeeds.
- **`startedAt`:** computed with the civil-date algorithm, not with `<chrono>` calendar support, which Apple's libc++ lacks.

### 21. The phase marker

The protocol layer owns it (`Watchdog`). The Dispatcher sets "Automation:<method>" and handlers refine it (`MethodContext::SetPhase`). How the script engine (M13) and the importers (M6) reach it is for their contracts to decide, for example through an `EngineContext`-owned marker. The crash breadcrumbs cannot serve, because they have no reader outside the crash handler.

### 22. Offloaded results

- **File names:** `<serverTag>-<sequence>.json`, with an 8-digit per-server sequence and the server tag `<processId>-<startSeconds>` (`MakeOffloadServerTag`). §13.4's `<id>.json` would put client-chosen strings into file names, and a bare sequence would let two editors sharing `user://Automation/Out/`, or a restarted editor in its project's `Library/Automation/Out/`, overwrite each other's results.
- **Without a project, and for read-only editors:** the files go to `user://Automation/Out/`.
- **Measurement:** the result's minified JSON without `_meta`.
- **Summary:** a small (< 4 KB) description of the top-level members.
- **Path:** the response names the absolute native path, which an agent can open.
- **Cleanup:** offloaded results are transient (an agent reads one right after its response). The first write to a directory removes the files of servers whose process no longer runs (the process id leads the server tag; `Process::IsRunning`), and the server's destructor removes its own files from every directory it wrote to, so neither `user://Automation/Out/` nor a project's `Library/Automation/Out/` grows without bound.

### 23. The MCP virtual environment

- **Owner:** `Tools/MCP/requirements.lock` belongs to stream D, with `Setup.py`'s virtual environment and the generated `.mcp.json`. The contract does not pin it.
- **Pins:** the official `mcp` SDK and its dependency closure, at exact versions.
- **Hashes:** for every distribution file of those versions, so one lock serves Windows, Linux and macOS.
- **Markers:** environment markers for platform-only dependencies (such as `pywin32` on Windows).
- **Generation:** a procedure documented in the file's header. Downloads are approved (`0001-approvals.md`).

### 24. Stream ownership

Streams own disjoint files (Roadmap rule 4). The tables below list each stream's sources; each stream also owns the tests of its units.

**Frozen headers.** Every public header this contract added or changed is frozen, and changing one needs the contract owner's review. The Roadmap names `Command.h`, `EditorContext.h`, `MethodRegistry.h`, the param-struct conventions (in `MethodRegistry.h`) and the provenance format (in `ProvenanceRecorder.h`).

**Private state is not frozen.** Every stateful frozen class declares an opaque `struct State; Scope<State> m_State;` (or the plain members it trivially needs), defined in its `.cpp`, so an implementation adds storage without editing the header. A `Scope` member has a non-trivial destructor, so Clang's `-Wunused-private-field` cannot fire while the bodies are stubs. Private data members and the `State` definition may be changed without the contract owner's review; public and protected declarations may not.

**A — commands, editor state and projects**

| | Files |
|---|---|
| EditorCore | `EditorContext.h\|.cpp` (with `EditorTransaction`, `EditorDryRunScope`), `Commands/*`, `Project/ProjectManager.h\|.cpp` |
| Resources | `Resources/Templates/Projects/**` |
| Tests | `Tests/Source/EditorCore/{EditorContext,Commands/*,Project/ProjectManager}Tests.cpp` (`Commands/CommandTests.cpp` included), `Support/EditorTestFixture.{h,cpp}` and its test |

**B — protocol, transport, the server and editor wiring**

| | Files |
|---|---|
| Engine | `Engine/Source/Engine/Automation/Protocol/**`; the socket additions of decision 27 (`Platform/Socket.h`, `Platform/Windows/SocketWindows.cpp`, `Platform/Posix/SocketPosix.cpp`) |
| EditorCore | `Automation/AutomationServer.h\|.cpp`, `Automation/BatchRunner.h\|.cpp`, `EditorCommandLine.h\|.cpp` |
| Editor | `Editor/Source/Editor/{EditorApp.h,EditorApp.cpp,EditorMain.cpp}` |
| Tests | `Tests/Source/Engine/Automation/Protocol/**`, `Tests/Source/Engine/Platform/SocketTests.cpp` (the new cases), `Tests/Source/EditorCore/{Automation/AutomationServer,Automation/BatchRunner,EditorCommandLine}Tests.cpp`, `Tests/Source/Editor/EditorAppTests.cpp`, `Support/{AutomationTestClient,ProtocolTestTypes}.{h,cpp}` and their tests |

**C — methods, validator and provenance**

| | Files |
|---|---|
| EditorCore | `Automation/{EditorMethodContext,AutomationTypes,RegisterMethods,ProvenanceRecorder,TranscriptLog,JsonPatchDiff}.h\|.cpp`, every `Automation/*Methods.h\|.cpp`, `Project/ProjectValidator.h\|.cpp` |
| Tests | their tests under `Tests/Source/EditorCore/` |

**D — Python client, MCP bridge and Python tests**

| | Files |
|---|---|
| Tools | `Tools/Automation/**`, `Tools/MCP/**` (including `catalog.json` regeneration and `requirements.lock`) |
| Tests | `Tests/Automation/**` |
| Guidance | `.claude/skills/add-automation-method/` (after this contract) |

**Shared files, one owner each:**

| File | Owner | Note |
|---|---|---|
| `EditorCore/Automation/RegisterMethods.cpp` | C | Roadmap |
| premake files (`premake5.lua`, `*/premake5.lua`) and `Dependencies.lua` | B | M4 needs no change: the projects glob their sources |
| `Scripts/ModuleRules.json` and `Scripts/Lint.py` | B | |
| `Scripts/Setup.py`, `Scripts/Test.py`, `Scripts/CI.py`, `.github/workflows/ci.yml` | D | the virtual environment, `.mcp.json`, the automation suite with method coverage and its skip check, the CI automation stage (on Linux under `xvfb-run` with openbox, like the unit stage: `test_launch_reports_editor_already_open_without_automation` starts a windowed editor) |
| `AGENTS.md`, `.claude/skills/build-and-test/` | D | at integration |

The contract implemented `App/EngineContext`, `App/Application` and `App/FrameLoop` (decisions 2 and 3). Further changes there go through B.

**Dependencies between streams.** A, B and C start in parallel, and D starts on the frozen wire format.

- B's server tests need C's session methods.
- C's method tests need A's editor state and B's server.
- A's provenance test needs C's recorder.
- Each stream's own unit tests come first, and the cross-stream round trips are un-skipped as the pieces land, as in M3 (ADR 0006 decision 20).

### 25. Where the acceptance tests live

| Roadmap M4 acceptance | File |
|---|---|
| "SceneEditCommand: 10,000 random operations undo to byte-identical JSON and redo to the final state" | `Tests/Source/EditorCore/Commands/SceneEditCommandTests.cpp` |
| "CompositeCommand: a failing child undoes executed children" | `Tests/Source/EditorCore/Commands/CompositeCommandTests.cpp` |
| "Framing: random byte streams never crash the decoder" | `Tests/Source/Engine/Automation/Protocol/FramingTests.cpp` |
| `test_batch_rollback_reports_failed_op`, `test_ref_substitution` | `Tests/Automation/test_batch.py` |
| `test_dry_run_*` (3) | `Tests/Automation/test_dry_run.py` |
| `test_if_revision_conflict` | `Tests/Automation/test_concurrency.py` |
| `test_bad_token_rejected_and_closed_after_three`, `test_http_probe_closes_socket`, `test_oversized_frame_closes` | `Tests/Automation/test_security.py` |
| `test_busy_watchdog_reports_phase` | `Tests/Automation/test_watchdog.py` |
| `test_meta_reports_new_warning`, `test_large_result_offloaded`, `test_enum_values_case_insensitive_and_echoed_canonically` | `Tests/Automation/test_meta.py` |
| `test_launcher_state_allows_only_project_methods` | `Tests/Automation/test_launcher_state.py` |
| `test_second_editor_on_locked_project_exits_3`, `test_read_only_editor_denies_mutations` | `Tests/Automation/test_lock.py` |
| `test_disconnect_cancels_pending_operations` | `Tests/Automation/test_disconnect.py` |
| `test_validate_fix_selected_ids_only` | `Tests/Automation/test_validate.py` |
| `test_scene_open_dirty_requires_save_or_discard` | `Tests/Automation/test_scene.py` |
| `test_provenance_records_method_request_and_transcript_line`, `test_upgrade_rewrites_and_records_provenance` | `Tests/Automation/test_provenance.py` |
| `test_launch_attaches_to_running_editor_with_automation`, `test_launch_reports_editor_already_open_without_automation` | `Tools/MCP/tests/test_launcher.py` |
| `test_transcript_written_without_env_var` | `Tools/MCP/tests/test_transcript.py` |
| `test_tool_schemas_are_compact` | `Tools/MCP/tests/test_schemas.py` |
| `test_run_py_reexecs_into_venv` | `Tools/MCP/tests/test_run_py.py` |
| SDK client conformance (catalogue without an editor, `editor_launch`, `entity_create`, `scene_tree`, `EditorCrashed`, `.mcp.json` without `python` on `PATH`) | `Tools/MCP/tests/test_conformance.py` |
| Method coverage | `Tests/Automation/test_methods.py` with the other suites; counted by `Scripts/Test.py` (stream D) |

### 26. Sharing the Runtime subset (M7)

§3 puts the handlers the Editor and the Runtime share in layer-5 `Engine/Automation/Methods`, and §13.5 serves the Runtime subset (`session.*`, `rpc.discover`, `scene.tree|query|get`, `entity.get`, `log.read`, `events.read`, and later play, input and observe methods) from there. M4 has no Runtime host, so the contract keeps those M4 methods in EditorCore, typed on `EditorMethodContext`. The mechanism M7 uses is decided now:

- **The protocol is ready:** a handler may be typed on any context type its hosts share (`MethodContext::IsHostType`, decision 7). Handlers that need only the registry can be typed on `MethodContext` itself.
- **M7's contract adds** `Engine/Automation/Methods/AutomationMethodContext.h`: an intermediate base of `MethodContext` with the host-neutral services as virtual functions (the target scene, entity resolution and summaries, the session report, the event log), which overrides `IsHostType`. `EditorMethodContext` then derives from it, and the Runtime's context does too.
- **M7's contract moves** the Runtime-subset param and result structs, their type registration and their handlers, unchanged in name and wire format, from the EditorCore domain headers into `Engine/Automation/Methods`, typed on `AutomationMethodContext`; the editor keeps registering them. The domain headers flag every such declaration ("their declarations move to Engine/Automation/Methods then").
- **Freeze:** the wire contract of these methods (names, members, semantics, errors) is frozen by M4. Their C++ location is not: the move is part of M7's contract task, reviewed by M7's contract owner, and needs no M4 review.

### 27. Sends never block the main thread

`ProtocolServer::Send` appends the framed message to the connection's outbound queue and returns at once. The I/O thread drains the queues with non-blocking writes, waiting for writability alongside readability, so the main thread (ECS, UI, every client) never waits on a socket and a client that stops reading delays only itself. A connection whose queue would exceed `MaxQueuedSendBytes` (64 MB) is closed and reported Disconnected. The I/O thread's own answers (Busy, handshake errors) use the same queue, so frames never interleave (ADR 0005 decision 16), and the same bound: a client that keeps sending requests whose ids those answers echo, without reading them, is closed too.

The M2 socket API sends only with a blocking `Send(data, timeout)`, whose timeout leaves an unknown part sent. The contract therefore appends to `Platform/Socket.h`:

- `Socket::SendAvailable(data)`: sends what fits right now and returns the count;
- `Socket::WaitAny(sockets, writers, listener, timeout)`: also reports writable connections in `SocketReadiness::WritableSockets`.

The existing members are unchanged. Stream B implements both OS halves (stubs and skipped tests until then). One I/O thread still serves every connection, as §4.11 states.

### 28. The editor's revision never repeats

`ifRevision`, `_meta.revision`, the revisions in results and history entries, and `scene.diff {against}` use `EditorContext::GetRevision`, not the open `Scene`'s own revision. A `Scene` object's revision starts at 0 and grows with the mutations of loading, so `scene.open`, `scene.new` and reloads restart it, and two scenes can report the same value. An `ifRevision` taken in one scene could then pass in another (ABA), and `scene.diff` revisions would go backwards. `GetRevision` is the open scene's revision plus a base that `SetScene` and `CloseScene` advance past every value reported so far, so it increases for the editor's lifetime.

### 29. scene.diff replays history entries on a scene copy

Decision 13 rebuilds older revisions with `SceneEditCommand::ApplyChanges` on a scratch copy, but `CommandHistory::FindCommand` returns a `const Command*`, and without RTTI nothing tells a `SceneEditCommand` from a `CompositeCommand` (a batch, a transaction, a validator fix). The integration therefore added one virtual to the frozen `Command.h`:

- `Command::ReplayOnSceneCopy(Scene& scene, bool after) const` brings a scratch scene from the state before the command to the state after it, or back, without touching the editor or the command. The default changes nothing when `ChangesScene()` is false (settings commands) and fails with Unsupported for a scene-changing command that does not override it, so a future command type cannot silently produce a wrong diff.
- `SceneEditCommand` applies its changes (`ApplyChanges`); `CompositeCommand` replays its children in order, or reverts them in reverse order, and refuses with InvalidState while only some children are applied (after a failed compensating step).
- `scene.diff {against: "revision"}` finds the target position from the held entries' `RevisionBefore`/`RevisionAfter`, copies the open scene through the serializer onto a scratch id generator, and reverts the applied entries after the target (newest first) or replays the undone entries before it (oldest first, the redo branch). History entries keep the revisions of their first execution (`CommandHistoryEntry`), so a revision names one state however often it was undone and redone.

Tests: "Command: the default replay changes nothing for settings commands and is Unsupported for scene commands", the two `CompositeCommand` replay cases, and "SceneMethods: scene.diff rebuilds revisions on both sides of the undo position, through batches".

### 30. Changes the contract owner accepted at integration

Each keeps public and protected declarations unchanged unless stated, as decision 24 allows:

- **`EditorContext.h`:** the private helpers `GetRevisionBeforeCommand` (the revision a `SceneEdit` started from, recorded as the history entry's `RevisionBefore`, which `scene.diff` needs) and `CreateProjectMount` (the dry run remounts the project with it after a failed overlay mount); private members for the provenance recorder, the dirty flag of a repaired scene and the pending revision; friend declarations for `CommandHistory` (reads `GetRevisionBeforeCommand`) and `SceneEdit` (sets the pending revision around its Execute). The friends widen access to private state, so they were reviewed: both classes are EditorCore's own command machinery, which the editor state already trusts (`EditorTransaction`, `EditorDryRunScope` and `ProjectSettingsCommand` were friends before).
- **`SceneEdit.h`, `SceneEditCommand.h`:** private members (the revision before the edit, the Debug-only entity snapshots, the cached memory size).
- **`CommandHistory.h`:** comments only. `IsDirty` treats the scene as dirty when a scene-changing entry lies between the save point and the position, or when the saved state can no longer be reached (it lay on a discarded redo branch or before a dropped record); dropping the record the save point follows leaves the saved state reachable, so that alone does not make the scene dirty.
- **`EditorApp.h`:** private overrides of `OnInitialize`, `OnShutdown` and `OnSafePoint`, a destructor and the opaque `State`; the factory's comment describes the editor options it now parses.
- **`JsonSchema.h|.cpp` (M3 Reflection):** the validator accepts the `required` keyword. Decision 8 puts `required` into params schemas, and the validator rejected any keyword the generator did not emit, so the frozen test "MethodRegistry: params schemas mark required members..." could not pass. The change only adds a keyword, with its own test case.
- **`Command.h`, `CompositeCommand.h`, `SceneEditCommand.h`:** the public `ReplayOnSceneCopy` of decision 29, the one public addition.
- **`MethodRegistry.h`:** convention 10's comment describes the virtual fields of decision 35; no declaration changed.
- **Test support:** `AutomationTestClient` takes `offloadLargeResults` (decision 31), and `EditorTestFixture` works in death-test child processes, where doctest assertions run outside a test case.

### 31. rpc.discover is bounded like every other result

The unfiltered `rpc.discover` (every method with its full params and result schemas, component `$defs` included) is far over the 48 KB threshold, so it is offloaded like any large result (§1.3 principle 7). There is no exemption: agents filter with `method` or `domain`, and the MCP bridge reads its tools from `catalog.json`, not from `rpc.discover`. The Python client gained `engine_client.load_offloaded(result)`, which reads an offloaded result's file and returns any other result unchanged; the tests that need the whole catalogue (`test_launcher_state_allows_only_project_methods`, `test_rpc_discover_matches_the_dumped_catalogue`) use it. The C++ round trip checks both forms: the default in-process client gets the offloaded summary, and a client connected with `offloadLargeResults` false (as `BatchRunner`'s is) gets the catalogue inline.

### 32. Float bounds are float values

`component.schema` reports a float field's `min`/`max` as the float value the validator compares (`FieldMeta` bounds of float fields are written with float literals), as `JsonSchema` rounds schema bounds. In-process the JSON holds that float widened to a double; the canonical writer prints it in its short float spelling (`0.001`), so agents see the authored value.

### 33. The commit gate and CI run the automation suite

- `PreCommit.py` runs `Test.py --suite unit,feature,automation --config Debug`: the automation suite drives the Debug editor (`ENGINE_AUTOMATION_CONFIG`), so every commit is checked by the tests of both languages without a Release build. It needs the MCP virtual environment (`Setup.py`); a missing one fails the step with exit code 3 and names `Setup.py`. §2.3, §15.9 and `AGENTS.md` say so.
- `CI.py`'s automation stage runs in Release (§15.8). `.github/workflows/ci.yml` runs it on all three hosted runners after the Release unit tests (on Linux inside the Xvfb + openbox step, because `test_launch_reports_editor_already_open_without_automation` starts a windowed editor). The setup stage creates the virtual environment with `Setup.py`; `actions/setup-python` caches pip's download cache keyed by the hashes of `Tools/MCP/requirements.lock` and `.github/ci-requirements.txt`, and `--require-hashes` re-checks every restored wheel. The virtual environment itself is never cached.

### 34. Implementation choices the streams recorded

- **Protocol (B):**
  - `ProtocolServer::Send` writes what fits at once without blocking when the connection's queue is empty; the I/O thread drains any remainder (decision 27 still holds: the main thread never waits).
  - A payload that is not a valid request, sent before `session.hello`, counts as an authentication failure.
  - Offload files hold indented JSON; the threshold is still measured on the minified result.
  - Every error an `edit.batch` op returns through `InvokeNested` (its params errors and the handler's own) is located relative to the op under `/params`, and `edit.batch` prefixes `/ops/<k>`.
  - A server without a sessions directory (in-process test servers only) names offloaded files by their `user://Automation/Out/` path; the editor always sets the directory, so its responses name absolute native paths.
  - The I/O thread logs rejected connections (bad tokens, HTTP probes, oversized frames, connection caps) at Warn, rate-limited (decision 6).
  - Batch ops run under the batch's phase marker, which reads `Automation:edit.batch > <op>` while each op runs.
- **Commands (A):** `SceneEdit`'s Debug check (§12.3: untouched entities are unchanged) compares each untouched entity's canonical JSON, the input of the state hash, instead of hashing it: the same result at about half the cost. The 10,000-operation property test carries `doctest::timeout(600)` because that check makes it take about 30 s in MSVC Debug (under 1 s in Release).
- **MCP bridge (D):** the bridge reads `ENGINE_MCP_USER_DATA_DIR` (the user-data root of editors it launches and of the session files it scans) and `ENGINE_AUTOMATION_CONFIG` (which build `editor_launch` starts; Release by default, then Debug; `engine_client.configurations_from_environment`, shared with the Python suite). Relative `project.create`/`project.open` paths are made absolute against the bridge's working directory. The transcript is locked per append, so two bridges number lines consistently. The transcript of a project being created or opened is pending: its lines are kept in memory, numbered after the lines the project's transcript already holds, and written only once the editor confirmed the project and its directory holds the `.eproj`; a refused call's lines are dropped. The bridge therefore creates nothing in a target directory first (`project.create` accepts an existing empty directory, and a refused call leaves nothing in a directory that is not a project). `engine_methods` reads the unfiltered `rpc.discover` from its offloaded file (decision 31). On exit the bridge shuts down an editor it launched only when nothing is unsaved. The pinned SDK is mcp 2.3.0.
- **Scripts (D):** `Setup.py` installs the virtual environment from wheels only (`--only-binary=:all:`) and recreates it when the lock or the Python version changes. `Test.py`'s automation suite runs both Python suites with the venv interpreter, writes JUnit with one test suite per directory plus one for the coverage gate, and fails on skipped tests (contract mode: `--allow-skips`).

### 35. Component values in params may set virtual fields

§13.10's example batch creates a light with `"Transform": {"EulerAngles": [-50, -30, 0]}`, and §5.4 makes `EulerAngles`, `WorldPosition` and `WorldRotation` writable virtual fields. They are not part of a component's stored JSON, so the struct validation of a component value called them unknown. The integration completed convention 10 in `MethodRegistry`:

- The validation pass takes the virtual fields out of every component value (on a copy of the params), checks each writable one against its own field (kind, length, range) at its pointer, and reports a read-only one (`WorldScale`, `RenderPosition`) as an error; the stored JSON that remains goes through the struct validation as before. The read pass is then not strict, because pass 1 already checked every member; the Variant values keep the virtual fields verbatim, and `entity.create`/`entity.update` apply them through their setters after the stored fields.
- Enum spellings of virtual fields are made canonical like those of stored fields.
- Full params schemas list the writable virtual fields in the component definitions they use (`MethodRegistry`'s component schema); results and `component.schema` keep the stored shape.
- An unknown member's "did you mean" suggestions still come from the stored fields only (Reflection's walk).

Tests: "EntityMethods: virtual component fields are set through their setters in entity.create and entity.update", the virtual-field checks in "RpcMethods: rpc.discover lists every method with schemas and flags", and `test_entity_create_sets_virtual_transform_fields`.

### 36. Changes the integration review made

The review of the integrated milestone found defects and gaps; the fixes are recorded with the decisions they belong to (5, 6, 8, 13, 15, 19, 20, 22, 27 and 34). The public declarations they changed, accepted by the contract owner:

- **Protocol:** `FrameDecoder` takes a per-instance payload limit (`explicit FrameDecoder(size_t maxPayloadBytes = MaxFramePayloadBytes)`, `SetMaxPayloadBytes`, `GetMaxPayloadBytes`); `ProtocolServerSpecification::MaxHandshakePayloadBytes`; `ProtocolServer::RejectionReportInterval`; `SessionFile::FormatUtcTimestamp` takes a `TimestampPrecision` (Milliseconds for transcript lines, so `TranscriptLog` no longer has its own calendar code); `MetaBuilder`'s `newWarnings` counts the Script logger's warnings too (they were counted nowhere); the `ifRevision` comments of `MethodRegistry.h` and `MethodContext.h` (decision 8).
- **Core and Platform (M1/M2 headers, additions only):** `ErrorCodeFromString` in `Core/Error.h`, the inverse of `ErrorCodeToString` that `BatchRunner` and the test client had each copied with a hard-coded last enumerator; `FileSystem::PathToUtf8` and `FileSystem::PathFromUtf8`, the public form of the non-throwing conversions of `Core/Private/NativePath.h`, which replace EditorCore's five copies (three of them threw on a name with an unpaired UTF-16 surrogate); `Process::IsRunning` (decision 22's cleanup).
- **EditorCore:** `SceneEditCommand::AppendChangeEvents` (decision 13); `AutomationServerSpecification::SessionFileRetryInterval` (decision 20); `EntityCreateParams::Name` has no default (decision 13); comments of `SceneEdit.h`, `ProjectManager.h` (the read-only cache's lifetime) and `EditorApp.h`.
- **Test support:** `EditorTestFixture` takes an optional type registration hook, so a validator test can register a component with an EntityRef field and one with a requirement other than Transform (M4's built-in components have neither); `Test::WaitUntil` (decision 15).
- **Logging:** EditorCore and the Editor use the client macros (`ENGINE_ASSERT`, `ENGINE_INFO`, ...), as CodeStyle §11 and Architecture §4.4 say; the automation server's connect, disconnect and session-file messages now reach the `App` logger that `log.read {loggers: ["App"]}` filters by.

## Requested amendments (docs owner)

- **Architecture §4.1:** `EngineContext` owns the `TypeRegistry` from M4, with `RegisterTypes` (decision 2).
- **Architecture §4.2:** the safe point `OnFrameSafePoint`/`OnSafePoint`, and batch runs unthrottled (decisions 3 and 4).
- **Architecture §12.3:** `GetMergeKey`, `ChangesScene`, `GetMemorySize`, the `Origin` accessors, `EditorTransaction` (nesting by joining), configurable history bounds, one history per open scene, and a fallible `Undo` (decisions 13 and 18); `ReplayOnSceneCopy` and the first-execution revisions of history entries (decision 29); the Debug check compares canonical JSON (decision 34).
- **Architecture §12.1 / §4.8:** the editor's revision (decision 28), and the id generator seeded from the OS when no state is given (decision 19).
- **Architecture §13.2:**
  - The I/O thread authenticates, with one hello strictness rule, Busy hellos during a stall, client slots for authenticated connections only, a handshake cap and deadline, a 64 KB frame limit before authentication and rate-limited rejection logs (decision 6).
  - Non-blocking sends through per-connection outbound queues (decision 27).
  - Session file permissions and rewrites (decision 20).
  - `ClientId`, `IMethodHost` (admission separate from invocation), `Dispatcher`, the typed handlers and `IsHostType` (decision 7).
  - Exceptions in handlers: Debug asserts, Release answers Internal (decision 7).
  - The protocol implementation choices of decision 34.
  - The I/O thread's own answers obey the outbound queue cap (decision 27).
- **Architecture §13.3:** the code mapping and the error `data` layout (decision 5).
- **Architecture §13.4:**
  - The param-struct conventions, string cursors with `"end"`, and `AllowedInBatch` (decision 8); every method accepts `ifRevision` (decision 8).
  - `ifRevision` compares with the editor's revision (decision 28).
  - `params._meta`, `result._meta` and `dryRun` in results (decision 5).
  - Dry runs use a scene copy, and read-only editors may dry-run (decisions 12 and 14).
  - Offload file names, directories and cleanup (decision 22).
  - The provenance key spelling (decision 10).
- **Architecture §13.5:**
  - `scene.new {save?, discardChanges?}`, `debug.pend` (not in the launcher state), the `scene.tree` JSON form, and the semantics of decision 13.
  - `Mutates` as decision 14 defines it.
- **Architecture §13.7:** the M4 validator codes, `ASSET_IMPORT_FAILED` for unreadable scenes, fixes limited to the open scene and settings, and diagnostic ids (decision 17).
- **Architecture §13.8:**
  - The bridge's environment variables, relative project paths, transcript locking, exit behaviour and pinned SDK (decision 34).
  - The transcript line format (decision 11).
  - The catalogue format and that C++ produces compact schemas (decision 9).
  - A headless editor listens except in one-shot runs (decision 14).
- **Architecture §12.1 CLI:** the M4 option rules (decision 14).
- **Architecture §2.3 `Lint.py`:** the Python contract rules (decision 16).
- **Architecture §2.3 `Setup.py` and `Test.py`:** wheels-only installs and venv recreation; the automation suite's runner, JUnit layout and skip rule (decision 34).
- **Architecture §13.4:** `rpc.discover` without a filter is offloaded like any large result (decision 31); float bounds are float values (decision 32); component values in params may set writable virtual fields (decision 35).
- **Architecture §3 / §13.5:** how the Runtime subset reaches `Engine/Automation/Methods` in M7 (decision 26).
- **Architecture §4.11:** the automation I/O thread also drains the outbound queues (decision 27).
- **Architecture §4.9:** the entity events scene edits, undo and redo append (decision 13).
- **Architecture §4.13:** the read-only editor's private cache is cleared at open and removed at close (decision 19); a failed session-file rewrite is retried (decision 20).
- **ADR 0004:** Python markers join the gate (decision 16).
- **CodeStyle §14 / AGENTS.md "Tests" / ReviewChecklist §8:** `test_busy_watchdog_reports_phase` is the Python suite's wall-clock exception, and bounded waits are not timing (decision 15). Applied by the integration review.
- **`FieldInfo.h` comment:** automation structs register camelCase field names (decision 8).
- **Roadmap M4 deliverables:** the files of decision 1, including those the streams added.
