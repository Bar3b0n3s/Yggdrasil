# 0019 — M13 scripting contract

- **Status:** implemented and source-reviewed; contract frozen by reviewed commit `8c066eb`. Milestone acceptance is recorded in the implementation commit's review trailer and requires both strict local gates below.
- **Date:** 2026-10-10.
- **Base:** `c95862a` (M9 rendering and M10 editor UI completed).
- **Authority:** Architecture §3–§7, §11, §13 and §15; Roadmap M13. Remote CI remains non-blocking under ADR 0011.

## Decisions

### 1. One VM owner and downward dependencies

`Sandbox` owns a single Luau VM, allocator, require cache and watchdog. Runtime `ScriptEngine` owns one Sandbox and the behaviour instances, tasks and error stream that use it. It never opens another VM to run an evaluation or replay expectation. Import-time extraction creates a fresh, independent Sandbox per import.

`IScriptHost` is the layer-4 boundary to the session: it exposes borrowed lower-layer services and frame state, plus checked mutation and deferred lifecycle requests. A Session adapter implements it. Scripting includes neither Session, Testing, App nor EditorCore. `IScriptTestHost` is the corresponding test service boundary; case collection returns retained function references, and the runner resumes a single case after normal tasks in phase 4. It does not drive the session recursively from a script wait.

`ScriptCall` is a private stack adapter. No public header names a Luau state or vendor registry reference. `ScriptReference` contains a VM generation and a never-reused reference index, both checked along with the internal reference kind. Release/cancellation is idempotent and cannot affect a newer scene's VM.

Luau's process state also owns the atomic monotonic VM identity counter. All hosted and standalone VMs draw from it; it is never reset on a ProcessContext restart within the same process, so retained opaque values cannot alias a newer VM. Exhaustion refuses creation. This identity is internal lifetime bookkeeping, excluded from simulation hashes, serialization and script-visible results; worker scheduling cannot change observable simulation state. It is the explicit §3 process-state exception needed to enforce cross-VM reference safety without pointer identities or unseeded gameplay randomness.

### 2. Process initialization and import threads

ProcessContext initializes Luau's released non-experimental flags and assertion bridge once, after crash handling and before physics, graphics and import workers. Workers never mutate these flags. Runtime VM calls stay on the main thread. Load-time VMs are explicitly thread-confined to their import worker; no scene, live host, mutable shared registry counter or runtime VM is touched. Type checking uses isolated per-request state under its thread-safe Asset-facing interface. This clarifies Architecture §4.11's main-thread scripting rule for the existing worker-based asset pipeline.

Registry registration is confined to the registering owner thread. Frozen metadata is immutable; runtime counters are main-thread-only and load-time imports never increment them. A pure load-time registry uses the same registrations and availability metadata as runtime bindings, without a second API list.

A standalone Runtime Sandbox used by tooling/tests may have no ScriptEngine host. It retains runtime allocator/deadline policy but only invokes registry members also marked available at LoadTime. Other members fail before native dispatch with an active-engine requirement; they never dereference a missing host. Hosted Runtime binding keeps the full declared runtime surface.

### 3. Persistent script data and schemas

`Asset/ScriptData` stores the authenticated extracted kind, name, compiler output, source map, recursive field descriptors and resolved require edges. It contains no VM, AST or frozen type-registry pointers. Script modules are standalone assets with their own stable handles, even when required by several behaviours. The importer records both dependency bytes and asset lookups.

`ScriptFieldSchemaSource` owns immutable reflected schema objects and pins their ScriptData. Reload publishes a new snapshot; any scene, editor operation or session using the old snapshot retains it. `Field.Array` retains its element descriptor recursively. `TypeInfo::ElementSchema` carries that element's metadata through generic reflection validation, JSON Schema and drawers; flattening it onto the outer array is incorrect.

Omitted field defaults are: Number/Integer zero, Bool false, String empty, Vector zero, Color opaque white, Quat identity, Entity/Asset null, Enum its first declared value, Array empty. Number uses the existing reflected finite f32 type and Integer the signed 32-bit type. Enum ordering is authored ordering; top-level field ordering is canonical name ordering. Unknown or mismatched overrides remain in the saved component and fall back to schema defaults at runtime, with diagnostics.

`ScriptDiagnostic` adds exclusive `EndLine` and `EndColumn`, preserving full type-checker ranges. A missing range remains zero; runtime stack locations do not invent unavailable columns.

`ImportContext` carries an owned `ScriptImportCheck` separately from warning-only asset diagnostics: performed status, environment fingerprint, root source hash and full findings. The manager publishes the latest attempt even when cooking fails, independently of the last good artifact. Successful cache manifests preserve it without changing cooked runtime bytes. A legacy/missing check is unchecked, and a checker fingerprint change invalidates checks and remembered failures. Provider replacement drains admitted jobs before rebinding. Handle-based import lookups are also recorded, including misses; replay scene identity resolves by handle after a move, and cache validation distinguishes those lookups from path queries.

### 4. Compilation, math and safety boundaries

The pinned compiler facade is shared by importers and development evaluation. Dist source compilation returns Unsupported before reading source or referencing compiler code; trusted cooked scripts and replay expectations execute through the same runtime VM.

Each distinct loaded chunk has a private VM identity, separate from its authored diagnostic label. Source-map lookup uses that identity, so a retained old coroutine keeps its original mapping after another eval or reload uses the same filename. Identical compiled bytes and complete maps may share an interned identity; differing maps never do. Maps remain available while their VM can execute their closures, and their metadata storage is bounded by the configured budget. Public errors and tracebacks expose the authored file/pointer/coordinates, not the private identity.

Compilation disables optimization assumptions about the rebound `math` library; sandboxing also clears the safe-environment optimization flag without removing readonly protections. DetMath and seeded randomness must remain effective through aliases and required/task threads. The `^` operator keeps Architecture §4.12's documented semantics; no AST or opcode rewriting is introduced. Nontrivial power expressions require the cross-configuration test, and gameplay guidance recommends `math.pow`.

The watchdog's name is `ScriptWatchdog` because Automation already owns `Watchdog`. Nested calls inherit the outer deadline. A GC interrupt never raises. Importing one complete require graph shares one 250 ms budget. The allocator tracks the first soft-limit recovery separately from a second breach and the hard limit, with 16 MiB headroom. No caller silently disables these limits.

### 5. Errors, read-only evaluation and deferred requests

Script errors are owned values. Deduplication keys are `(script, jsonPointer, line, message)`; every occurrence receives a new monotonically increasing cursor ID, including an update to an existing entry's count. Reads return updated entries after an exclusive cursor in ID order. This makes repeated faults observable without duplicating the visible error list. Embedded replay code retains its real replay path, `/Expect/<index>/Luau` pointer and authored Luau line/column; generated expression wrappers never leak their line offset into diagnostics. Compile requests distinguish chunks, expressions and expression-or-chunk input. The compiler first recognizes a complete expression when requested; otherwise it compiles the authored chunk, including an existing `return`, unchanged. Virtual eval chunk names confer no filesystem access.

Read-only edit evaluation rejects host mutations before native callbacks or write helpers perform a side effect. That includes advancing or reseeding the session's random stream. Local tables, local generators and local Color/Quat values remain usable. Availability, mutation policies, signatures, declarations and coverage all come from one API registry.

Quit and fatal-stop requests are consumed after protected calls unwind. Scene.Load is applied at frame end and gives the new scene a fresh VM and generation; it preserves the session serial, absolute tick/input timeline and lockstep/replay ownership. The session host reports environment/cursor state without an App or editor include in Scripting.

### 6. Test lifetimes, recording and audio

`Test.ExpectScriptError(pattern, withinTicks)` matches an unclaimed nonfatal occurrence observed since the current case began, or waits for one through its simulation-tick deadline. Matching uses Luau string-pattern rules against the message and validates the pattern before waiting. A match claims exactly one occurrence through `IScriptTestHost::OnExpectedScriptError`; it never removes logging or error counts. The runner classifies unmatched faults when the case ends. Setup/teardown faults, fatal stops and a fault of the case body cannot be claimed. Claims cannot cross VM/case lifetimes.

`Test.Suite` options accept `CaseTimeoutTicks` (positive integer) as the default for its registered cases; an individual `Test.Case`'s `TimeoutTicks` wins, then the suite default, then the project's `Testing.CaseTimeoutTicks`. Omitted or empty options inherit. Unknown options are located errors. Collection resolves the suite default into each returned case descriptor, so recollection compares the effective options without another public options type.

A case requesting Scene.Load must return before the frame-end replacement. A case still suspended at replacement receives a located error and its old coroutine is cancelled. Suite isolation then re-collects the suite in the new VM, verifies the complete ordered case identities and options, and continues at the next case; completed bodies never rerun. Case isolation instead restarts the configured scene before the next case. Results, error counts and coverage live outside the VM, and setup/teardown errors are forwarded to the test host before VM destruction. A scene-dependent changed case list is an error. Quit takes precedence.

`test.run {record}` publishes only after successful selected cases and fresh ordinary-play verification with the candidate replay and strict final hash. The verifier has no test driver or test host. Case-isolated session segments retain all applied events with cumulative tick offsets; their resets are not encoded in v1. Clock and isolation modes are accepted, but a candidate that cannot reproduce the original final hash returns Validation and is not written. The recorded final hash is never replaced by the verifier's hash. A reproducible starting scene asset is required for recording, while ordinary suites still support empty scenes. Known test-driver mutations or session RNG consumption invalidate a recording even if the final effect cancels out. Verification is cancellable, reports its phase and counts against an explicit run-wide tick limit, but not the original cases' ticks or coverage. This proves input/explicit expectations and terminal state, not arbitrary serialized test closures.

Windowed Editors construct AudioEngine with deterministic decoding from startup, while retaining their selected playback device. This avoids changing resource managers under loaded clips when a test starts; the tradeoff is inline decoder work in ordinary editor playback. Ordinary windowed Runtime retains threaded decoding; test entry points select deterministic decoding before creating the engine. Every suite and replay verification owns simulation audio time for its complete lifetime.

`Test.CaptureAudio(n)` measures exactly n ticks starting with the current fixed step. The runner uses the existing capture buffer and absolute sample boundaries `round(tick * 48000 / FixedHz)`, trimming samples for earlier/later steps grouped in the same frame. It never performs an extra pull. When the containing frame's AudioUpdate has not run at the logical wake tick, the test thread remains suspended until the next phase 4 after the samples exist. Cancellation and case teardown stop capture before releasing audio time. `IScriptTestHost` exposes BeginAudioCapture(ticks), IsAudioCaptureReady, EndAudioCapture and CancelAudioCapture for this protocol; no second audio engine is introduced.

### 7. Automation output and ownership

`input.replay` returns per-expectation outcomes and locations, finalTick, stateHash and hash-verification status. Verification failures retain the full outcome in structured error data. Admission rejects another time owner or active driver before replacing the session; every terminal path immediately releases the operation's input suppression, stepping and temporary time ownership, guarded by session serial. `input.record` registers no pending operation, so its host independently cancels the recorder on disconnect and publishes nothing.

Recording's Start only changes transient play state. Stop checks write authorization explicitly before consuming or writing the recording, matching `test.run`'s optional recording policy; the method-level Mutates flag is false. Editor paths remain confined to project://. Exported Runtime's project:// is a read-only pak, so Runtime recording has a narrow exception: relative output paths are confined below user://Replays/, returned as that canonical identity. Runtime replay accepts that returned identity or a project:// replay asset. Absolute paths, traversal and every other scheme are rejected. Development Runtime may compile source expectations; Dist uses cooked replay bytecode and exposes no development automation.

play.waitFor predicates use the live VM with external-driver origin. Host writes and session RNG consumption notify OnExternalMutation before the side effect, including a call that later faults. Recorded Test.Inject input and ordinary gameplay callbacks are not external mutations. Origin follows deferred script work; nested calls invalidate at most once per execution slice.

### 8. Contract ownership and completion

The parent owns the host, ScriptEngine, TaskScheduler and error contracts, shared existing files, documentation, build integration and the final contract review. Disjoint contract tasks own VM primitives; registry/helpers/proxies; persistent script assets/importer/type-checker; test runner/replay; and automation/editor script services. The exact public headers and marked stubs are reviewed together before freezing.

Only the contract commit may retain `ENGINE_CONTRACT_STUB` and skipped acceptance skeletons. Implementation must replace every one, complete all §11.5 APIs and editor/automation parity, and pass strict PreCommit plus full local Windows CI before M13 is reported complete. Existing M9/M10 behavior remains covered by the contract gate.

### 9. Approved integration amendments

The contract owner approved these additions while integrating the implementations:

- Parameter validation pins an immutable script schema snapshot for the whole request. The method context can complete a partial entity update's missing script owner from the addressed entity before external-variant validation; supplied owners are never replaced. This permits fields-only patches while retaining range, type and script-kind validation before mutation.
- Windows projects select x64 host tools at workspace scope. The M13 Debug Tests link exhausted the 32-bit linker's address space despite available system memory. Premake's `preferredtoolarchitecture "x86_64"` emits `PreferredToolArchitecture=x64`; CheckBuildConfig requires it and a seeded 32-bit-host fixture proves the regression is detected. See [Premake](https://premake.github.io/docs/preferredtoolarchitecture/) and [Microsoft's MSBuild properties](https://learn.microsoft.com/en-us/cpp/build/reference/msbuild-visual-cpp-overview?view=msvc-180). Target architecture and floating-point settings are unchanged.

- `PlaySession::GetScriptErrors` retains a session-wide error stream across scene replacement. A new VM does not reset automation cursors or erase errors the suite still needs to classify. Quit or fatal stop raised during old-scene teardown cancels the pending replacement before the new scene starts.
- The editor script service offers a const checking operation and immutable schema snapshots. Scene commands, prefab operations, duplication, scene loading and export pass the same schema source through reflection. Only fields declared as entity references are remapped; strings containing UUID text remain strings. Assigning a Module or TestSuite as a behaviour is rejected before mutation.
- Export checks scripts regardless of `BlockPlayOnTypeErrors`, which controls entering Play. Script memory statistics are available through the shared editor/Runtime method only while a live VM exists.
- Reload evaluates candidates with a fresh require cache and load-time API restrictions. Validation cannot mutate the live scene or reuse mutable live closure upvalues; failed candidates preserve the active instance and module graph.
- `GenerateDocs.py` lands in M13 for the required scripting declarations, scripting reference, automation method schemas and MCP catalogue. The commit gate checks these outputs after building the Debug editor; CI checks them after each non-Dist build. M14 extends this generator to its remaining planned references.
- Native extension tests use the public binding facade: an optional synchronous `RegisterBindings` configuration hook runs before freezing, and `LuaHelpers` exposes checked task handles, the borrowed engine pointer and structured error forwarding. Tests and editor tooling never include Scripting's private headers. The process initialization query is public through `Sandbox.h`; it exposes no Luau state.

### 10. Implementation review amendments

The contract owner approved the following corrections found during independent implementation review and integration verification. The final gate below remains required.

- Native values converted from Luau use a separate allowance of `min(4 MiB, VM soft-limit bytes)`. Each expanded value and table entry costs 256 bytes, and strings and keys cost four bytes per source byte. Repeated aliases are charged on every expansion before allocation. JSON and reflected-value conversion share this policy and poll the inherited deadline; a script `pcall` cannot clear a captured safety fault. Exhausting the native allowance is a structured runtime input error and does not count as a tracked VM allocator breach. Valid subsequent calls remain usable.
- `ScriptEngineSpecification::ClockSeconds` is an optional copied callable with the same finite, nonnegative, monotonic and capture-lifetime contract as the sandbox clock. It exists to exercise hosted watchdog boundaries deterministically; it is never simulation time and defaults to `lua_clock`.
- Export fails when the checker cannot refresh its environment; it never treats an unavailable service as a clean type check. Script reference validation resolves effective schema defaults and overrides recursively, pins the schema version for a fix, and preserves unrelated legacy overrides during undoable repairs.
- The runtime accepts the manifest's `Testing:true` now that its cooked-script runner exists. `--feature-test` still rejects ordinary manifests. Without an explicit development user-data root, a testing run keeps logs, crashes and `user://` below `<export>/bin/TestUserData/<game name>`, including Dist. Ordinary games retain the platform user-data location. The full testing-export option and automatic three-mode coverage gate remain M15 work; M13's integration fixture includes test assets and sets the existing manifest flag explicitly.
- `PlaySession::Prepare` performs fallible scene, input, physics and empty-VM preparation without gameplay callbacks, voices or shared mixer/time ownership. A failed or discarded candidate preserves the original session. `Activate` runs once after retiring the previous session, acquires audio, initializes instances and runs startup callbacks, then starts PlayOnStart voices. `Create` remains Prepare followed by Activate. Startup script faults use the normal script-error path. Replacement adapters configure pause, lockstep and validated time scale before activation and preserve startup changes.
- `AudioSystem::InitializeMix` idempotently saves Music/Sfx/Ui and resets them to one before startup scripts can read or change gains. `Start` invokes it for standalone callers without resetting an already initialized mix. Teardown restores the saved mix even if no voices started. Scene replacement follows the same ordering; Master remains host-owned.
- Scripted-clock nonprogress uses the case's effective `TimeoutTicks` as a separate limit on consecutive completed frames with zero fixed steps. Frame callbacks run before this check, and a stepped frame resets the counter. At the limit, a still-running case times out with its source location, cancels its thread/audio capture and ends the suite. Finite pauses may recover; reported ticks and the existing tick budgets continue counting only simulation ticks. An all-zero authored clock remains invalid. A quit first observed during final teardown is classified exactly once against `ExpectQuit`, with a reported quit row even after all case bodies passed.
- Replay playback retains future authored events in its own storage and queues only the next tick's events. Cancellation drops unconsumed events while preserving independent input and the already-applied boundary state. An applied Tap still releases on the next tick before that tick's authored events; explicitly held buttons and axes remain until subsequent input or `input.inject releaseAll`. The caller pauses and releases its own input/time lease. The complete stream is validated before playback begins.
- Reapplying an unchanged project setting preserves byte-identical project files instead of atomically replacing them. The prior replacement changed the file fingerprint with no settings change for autosave to recognize, so the next Play could report an external-edit conflict. Write permissions, command history and revisions retain their existing behavior; genuine external fingerprint changes remain conflicts. The scripting policy regression and an autosave undo/redo regression cover this seam.
- `EditorPlayController::OnSafePoint`, `OnAssetReload` and `OnInputEvent` connect the editor host to queued script reloads, deferred quit handling and exclusive input ownership. Reloads finish dependent publication before applying each transitive chain once; failed imports preserve running code. Ending a deferred session drains its released imports before another Start. Ordinary quit runs the normal Stop path only after UI/RPC/batch borrows unwind. Test, replay, recording and lockstep owners retain control.
- `EditorContext::AcquireScriptService` returns the actual checker configuration or infrastructure failure, allowing configured Play checks to fail before replacing a session. The existing nullable getters remain for optional callers. Both Edit eval and Play read live environment/cursor state from the borrowed window, including the null-platform headless window; the documented no-window fallback remains. Stop, quit and replacement restore cursor ownership, and native/viewport input never overwrites an exclusive test or replay stream. These changes are covered through both real editor automation and native/headless window tests.
- `IScriptHost::RequestSceneLoad` takes owned JSON parameters by value; the binding and host move them into pending session storage. Native conversion and push traversal use heap frames at the accepted depth boundary. This avoids recursive deep copies through the JSON library after successful conversion. A real session regression transfers a maximum-depth parameter object across scene replacement and reads it in the fresh VM, retaining isolation from later Lua table edits.

## Review follow-through

The local agent environment omitted Windows' `PROCESSOR_ARCHITECTURE` and `PROCESSOR_ARCHITEW6432` variables. MSBuild therefore reverted even an explicit x64 preference to x86. The host was verified as x64 and the standard `PROCESSOR_ARCHITECTURE=AMD64` value restored in verification processes; no installed tool configuration was changed.

The interface review identified and corrected missing per-expectation replay outcomes, incomplete cancellation ownership, embedded-source attribution and script-array element metadata. The generated declarations must be parsed with the pinned New solver during implementation; optional bounds returns use `(vector?, vector?)`, since an optional result tuple is not Luau syntax. Acceptance tests cover each decision above. Their contract skips are temporary and claim no implementation result.

The independent source review covered every new file in full and the relevant shared hunks against ReviewChecklist §0–§13. Its sixteen concrete findings (including four groups of static findings) and the contract corrections are:

| Finding | Correction |
|---|---|
| Replay response omitted required expectation outcomes/final tick | Typed outcomes, locations and hash status, retained even on validation failure |
| Replay admission and terminal ownership were incomplete | Preflight driver checks, serial-guarded immediate cleanup, recording disconnect obligations |
| Factory constructors were inaccessible to CreateScope/CreateRef | Public construction-key pattern for ScriptEngine, Sandbox, TrackingAllocator, ScriptTypeChecker and ScriptFieldSchemaSource |
| Standalone Runtime Sandbox contradicted binding requirements | Explicit metadata-driven pure subset without an engine host |
| Reused diagnostic labels could select a newer chunk's source map | Private loaded-chunk identities retaining the original map |
| StartPending's comment incorrectly excluded unstarted instances | Separate startup snapshot and started-only update contracts |
| Import boundary lost full check results and cache fingerprints | Context-owned attempt result, immutable provider fingerprint, cache/publication metadata |
| Replay importer could not record authoritative handle lookups | Recorded handle queries, misses and cache revalidation contract |
| script.errors discarded embedded-source pointers | Typed JsonPointer in the RPC result with cursor regression coverage |
| Test reports discarded embedded-source pointers | JsonPointer on flat cases and individual failures, JSON/JUnit retention coverage |
| StartPending named UUID order instead of canonical hierarchy order | Contract now matches §11.3; callback acceptance explicitly distinguishes the orders |
| Unused private builder storage failed Clang's warning gate | Removed speculative fields; implementing owners add storage when they use it |
| A test's partial ResolveContext initializer failed Clang's warning gate | Value-initialize the complete context, then assign its schema source |
| Eight replay/test-result contract files were not format-clean | Applied the repository formatter to those files |
| Import check metadata collided with the automation ScriptCheckResult type | Named the Asset-facing record ScriptImportCheck; preserved the RPC result name |
| The test host could observe faults but not learn which occurrence a case expected | Explicit same-case claim callback, preserving logs/counts and rejecting fatal/setup claims |

Checklist applicability: §0 and §8 use the explicit contract exception for marked stubs and skipped acceptance skeletons; no runtime acceptance is inferred from them. §1–§7 and §9–§10 cover the declared boundaries, ownership, determinism, errors and documentation. §11 has local compiler verification only; Linux/macOS are not claimed verified. §12 has no vendor or build-setting changes. §13 covers confined paths, read-only checks and sandbox availability; transport/authentication code is unchanged. Full implementation review and strict gates remain required before M13 completion.

## Implementation review

Three independent implementation reviews covered the VM/bindings, session/testing/runtime and editor/assets/reflection/automation. Each reviewer read the new files in their scope and the shared changes in context against ReviewChecklist §0–§13. The contract owner reviewed their fixes, the shared integration changes, build scripts, generated references and regression results. All fourteen concrete findings below were corrected.

| Finding | Correction and regression |
|---|---|
| Small shared Lua tables could expand into unbounded native JSON | Shared native conversion budget and inherited deadline, including typed reflection; DAG, string/key, cycle, depth and recovery cases |
| Zero time scale bypassed scripted-clock timeouts | Consecutive zero-step frame limit; zero-scale startup/case failures and finite-pause recovery |
| Final teardown quit could falsely pass a suite | Classify after destruction exactly once; suite/case isolation and expected, unexpected and mismatched quit cases |
| Replay cancellation left future input in the session | Player-owned future events; cancellation, independent input and Tap-release tests plus both host disconnect paths |
| Startup scripts observed the preceding audio mix | Initialize the owned mix before callbacks; startup and scene-replacement mix tests |
| Runtime restart activated a candidate before retiring its predecessor | Prepare/Activate split; failed preparation preserves the original, successful exported restart restores ownership in order |
| Ordinary editor saves never reloaded the live VM | Safe-point reload queue with import completion, dependent-chain rollback and deterministic-owner deferral; real editor save/refresh tests |
| Editor script quit left ordinary Play active | Consume after dispatch through normal Stop; startup, update and eval quit tests with selection/cursor restoration |
| Configured type errors did not block editor Play | Refresh and check before admission/replacement; behaviour/module and checker-infrastructure failure tests |
| Script environment queries ignored the editor window | Borrow live window state; actual native and headless window regressions |
| Script cursor state was detached from window ownership | Apply to the window and restore each owner's baseline; Stop, blur, quit, test lease and replacement regressions |
| Native input could modify an exclusively driven test | Guard both event ingress and viewport blur; ScriptedClock hashes and authored-input assertions with injected native noise |
| Export could omit checking when the checker was unavailable | Fail validation with a located cause; invalid checker configuration regression |
| Functional casts violated the repository's C++ rules | Explicit casts in checker/cache tests, covered by compilation and lint |

Central verification also corrected stale test assumptions about offloaded reports, published asset versions and serialized entity response shape, completed the generic Script/Replay importer sweep, admitted testing manifests in the real exported runner, and fixed the identical-settings/autosave conflict. Deep conversion tests exercise the accepted nesting boundary without increasing the native stack size. No acceptance test is skipped or weakened to close these findings.

The first full strict run also caught duplicated physics diagnostics: Physics logged them once, then the new scripting listener logged them again while publishing the script error. Forwarded diagnostics now retain the complete script error stream, test reporting and pause/instance policy without emitting a second log entry. The existing physics acceptance test keeps its exact log counts and additionally checks that all four refused entities appear once in the script error stream.

Checklist disposition: §1–§10, §12 and §13 have source fixes and regression coverage; §0 additionally requires strict PreCommit and full local CI before the implementation commit. §11 requires the local MSVC/clang-cl matrix and generated-platform checks, while Linux/macOS execution remains remote-CI verification under ADR 0011. Vendor sources are unchanged. M14's full FeatureTest coverage gates and M15's testing-export option remain in their owning milestones.

## Verification

The contract commit's `Reviewed:` trailer records its PreCommit result and contract mode. The implementation commit records strict PreCommit, full `CI.py --require-clang-cl`, and the exported configuration audit; a preliminary or contract-mode run never substitutes for those gates. Development logs remain under the ignored `bin/M13-*` paths.

The final native-conversion regression run passed all 47 selected Sandbox, LuaHelpers, LoadTimeVm and real-session parameter-transfer cases (5,126 assertions), including maximum-depth values, aliased tables, deadline propagation and recovery after rejected input. The editor host and autosave run passed all 56 cases (1,126 assertions). These targeted runs supplement the complete suites in the gates.

`Tests/Automation/test_script_modes.py` exports one authored project, executes cooked scripts through the standalone runner, and compares its final state hash with the editor. It folds 96 seeded nontrivial power expressions into visible scene state and checks known fractional powers plus fixed-step motion. The ordinary automation suite checks its own built configuration; the milestone audit invokes the same helper for Debug, Release and Dist after all three executables are built. Dist runs the cooked artifacts without development evaluation. The fixture enables the testing manifest flag without modifying either pak; the public testing-export option remains M15 work.
