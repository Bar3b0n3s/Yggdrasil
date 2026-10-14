# 0025 — M15 completion review

- **Status:** accepted with the M15 milestone commit (lead-owned), after the single `CI.py --milestone` run on the final tree (ADR 0023 decision 2).
- **Date:** 2026-10-14.
- **Base:** `de9f89a` (the M15 contract) plus the staged M15 implementation and its review fixes in the milestone worktree (ADR 0024 "Integration").
- **Authority:** Roadmap M15 acceptance ("a completion review checks every section of `Docs/Architecture.md` against the code and records any deviation as an ADR"); ADR 0024 decision 21 (the review, its row per subsection and the items it must dispose of); ADR 0011, ADR 0022 and ADR 0023.

## Context

M15 completes the engine and editor. Before the milestone commit, every section of the Architecture (§1 to §18 and appendices A to D) was checked against the code, so that the authoritative document describes what the engine does, and every place where it does not is either fixed, accepted with a reason, or deferred with an Appendix C row.

### Method

- **Five readers**, read-only, each owning a part of the document: §1 to §4; §5 to §7; §8 to §10; §11 and §12; §13 to §18 with appendices A to D. Each compared every concrete statement (names, signatures, numbers, orders, file paths, test names) with the code of the staged tree, and read the ADRs that had requested amendments to its sections (0002, 0003, 0005 to 0016, 0018, 0019 and 0021 to 0024). Nothing was built or run; behaviour was established by reading code and tests. Each reader listed what it checked fully and what only partly (for example the shading math of §8 was spot-checked against the Slang sources, not derived, and the editor's interaction details of §12.2 were not traced).
- **The milestone review** ran beside it with three lenses (export; hardening; CI, portability and documentation) over the M15 changes.
- **Adversarial verification.** Two verifiers tried to refute every candidate against the code, the vendored sources and the documents, and reclassified where the evidence said so (A026 and A065 from deliberate to deferred; A023 kept as plausible).
- **Result:** 82 Architecture deviations kept (A001 to A084; one major, 47 minor, 34 nits: 64 stale texts, 7 code defects, 6 deliberate deviations, 5 deferrals), 33 milestone findings kept (M01 to M35; 18 minor, 15 nits), and 4 refuted (M04, M19, A050, A064). The records are kept under the ignored `bin/Reviews/` (`M15-Review.md`, `M15-Review.json`, `M15-FixDecisions.md`, `M15-GuardProofs.md`).
- **One systemic cause.** Most deviations were "Requested amendments" that earlier ADRs (0003, 0005, 0006, 0008 to 0010, 0012) listed for the docs owner and that never reached the Architecture. Each kept one is applied in this change (decision 14); an ADR's amendment list is not a substitute for the amendment.

The lead decided every kept finding (`bin/Reviews/M15-FixDecisions.md`): nothing is dropped. Code fixes went to the editor, engine and Python agents; the documentation amendments are applied in this change.

A review of those fixes (`bin/Reviews/M15-FixReview.md`: seven C++ findings, C1 to C7, and ten Python and documentation findings, P1 to P10; no blocker and no major) was applied in the same change. Its results are folded into the items below (A024, A025, M02, M11, M12, M14, M23, M33, and decision 14's §3 table), each naming the finding.

## Decisions

### Deviations accepted

#### 1. The profiler's timeline export and GPU merge have no surface in v1 (A004)

`ENGINE_PROFILE_SCOPE` records CPU zones into per-thread rings (the frame loop and the job system use it), and `stats.get` reports per-pass GPU times straight from `GpuProfiler`. `Profiler::SubmitGpuZones`, `CollectZones` and `ExportChromeTrace` exist and are unit-tested, but nothing calls them. No editor panel, agent workflow or test consumes a merged timeline or a Chrome trace, and exposing one would add an automation method, its schema, coverage and Python test for no client. ADR 0024 decision 16 left the choice to this review: §4.13 now describes what ships, and an Appendix C row keeps the export and the merge as a later decision.

#### 2. No per-frame arena (A005)

The render snapshot is extracted by value into standard containers every frame (`ExtractRenderSnapshot` returns it), and a session's debug draw list keeps its capacity across `Clear`. No measurement points at allocator pressure (the M15 perf capture shows no metric over its budget, ADR 0024 "Integration"), and an arena would give the snapshot a lifetime rule that the renderer, the editor's captures and the tests would all have to respect. §4.7 now names the allocators in use, `TempAllocatorImplWithMallocFallback` for physics included; Appendix C keeps the arena. The review's suggested wording said that the snapshot's containers keep their capacity across frames; they do not, and the amendment says so.

#### 3. Numbers a writer cannot write back, and `.meta` members (A028)

An unknown component is preserved on save, but `JsonWriter` writes every fractional number as a `float`, so a fractional number outside `float`'s range could not be written back and would make the scene unsavable, against §6's byte-identical load and save. Such an unknown component is therefore `SCENE_INVALID_COMPONENT`: an error in a strict load, dropped with a `LoadRepair` by a repair load. The same rule refuses such numbers in a `.meta` file's `Settings` (Validation). `.meta` files also refuse unknown members, because they are engine-written: a member a hand edit added is a mistake to report, not data to preserve. §6's strict-reader bullet states both.

#### 4. The type checker reads the registry's definitions, not the committed file (A052)

`ScriptTypeChecker` loads the definitions that the live `ScriptApiRegistry` generates in memory (`GenerateDefinitions`, under the chunk name `Engine.d.luau`). The committed `Resources/Scripting/Engine.d.luau` is never read at run time, so a stale or missing copy can never change a diagnostic, and `GenerateDocs.py --check` keeps the committed copy byte-equal to the registry for readers and tools. §11.9 is amended.

#### 5. Luau's compiler objects stay linked in Dist (A063)

ADR 0024 decision 11 asked stream D to remove the Luau compiler from Dist if that needed no vendored-source edit and no behaviour change. It needs one or the other: the require library's configuration parsing (`extractLuauConfig`, which calls `Luau::compile`) and `ScriptApiRegistry`'s parsing of first-party signatures (`Luau::Parser::parseType`) reach the `Compiler`, `Parser` and `Ast` objects. No game input reaches them: `ScriptCompiler` returns `Unsupported` in Dist, only cooked bytecode loads (verified by hash before `luau_load`, §14.1), and `get_config_status` is always `CONFIG_ABSENT`. Removing them would need an edit of vendored source (forbidden) or a Release/Dist behaviour difference (against the parity and determinism rules), so no `LuauCompiler` family was added to the map check. §14.4 now says "no reachable source compilation"; Appendix C keeps the stripping.

#### 6. `EngineVersion` stays the constant `0.1.0` (A065)

ADR 0008 decision 5 left build versioning to the export milestone. A per-build value (for example `<semver>+<commit>`) would change every replay header and every session file at each commit, for no consumer in v1. `Game.json`, `Engine.pak`'s metadata, replays and session files carry `0.1.0`; §14.1's example is corrected, `JsonRpc.h`'s comment states the v1 rule (editor agent), and an Appendix C row keeps the scheme open.

#### 7. FeatureTest's 24 copied-in golden files keep no provenance (A069)

The nine M9 golden scenes and the three `RendererII` materials, each with its `.meta`, were copied into FeatureTest from a unit test's replay of `Golden.jsonl`'s last 30 requests, not written by an editor run of the project, so `Provenance.json` has no entry for them (ADR 0022 review fix M11 explains why an invented entry would be false). FeatureTest is never audited: `AuditProvenance.py` targets the demo projects. The gap is accepted until a reviewed regeneration of the golden content through one recorded editor run, which any audit pointed at FeatureTest would first need; §15.4 says so, and an Appendix C row records it.

#### 8. Editor viewports resize on the frame the panel changes (A039)

§8.1 promised a 2-frame debounce of the editor's viewport render targets. M10 never built one: `EditorHostViewports::SetRectangle` takes the new size on the frame the panel's rectangle changes, cancels pending picks and bumps the view's generation, and the picking and published-extent rules of ADR 0018 and ADR 0021 rely on that immediate resize (ADR 0018 accepts that a resize may need a second frame). A debounce would make the published pixel size lag behind the panel for no measured gain. §8.1 is amended to the immediate resize.

### Deferrals

#### 9. Editor shader hot reload is deferred past v1 (A026, A038)

No milestone ever scheduled it, and nothing watches `Resources/Shaders`, runs `CompileShaders.py` or rebuilds pipelines. Shaders change only with engine code, after which the editor is rebuilt and restarted anyway. §7.5 and §8.12 now say that shaders do not hot-reload, §14.4 keeps "no asset hot reload" for Dist (the refuted A064 is that sentence, reworded with this decision), and an Appendix C row holds the feature. `ShaderLibrary::Clear`, whose comment named a hot-reload consumer and which only its own tests used, is removed.

#### 10. `AuditProvenance.py` and the games suite and stage are M16 deliverables (A022, A070)

The audit script and the `games` suite and stage were named only in the acceptance of M16 to M18, so no milestone owned building them. Roadmap M16 now lists them among its deliverables. §2.3 marks the script "(M16)", §15.8's games row says it arrives with M16, and §13.12 no longer says that T5 runs the audit on every commit: the games stage runs it, at the milestone gate (§15.1, §15.8), and whether PreCommit gains the games suite is M16's decision.

#### 11. A character is not carried by moving ground (A042)

`CharacterController` adds only the ground's vertical velocity, and no binding exposes the ground's velocity. §9.6 said "open for M13's bindings", a finished milestone. No demo depends on a carried character (Rolling Ball's ball is a Dynamic body), so the behaviour is recorded as v1: a game adds the platform's velocity to its own `Move`. §9.6 is amended and Appendix C holds the feature.

### Code fixes

#### 12. Fixes of the completion review

Each comes with its test, and each bug fix with a regression test proven to fail without it (ADR 0023 decision 4):

- **A002, A071:** `GenerateDetMathReference.py --check` joins the static checks as the step `detmath reference` (after the fixture generators; `--check` runs the script's self-test first), so `PreCommit.py` and `CI.py`'s lint stage staleness-check the committed `Tests/Source/Engine/Core/DetMathReferenceData.h`; proven with a one-byte edit of the header. This closes ADR 0007's follow-up (decision 17).
- **A003:** `ProcessContext` and `ImGuiLayer` stop converting paths with `path::u8string()`, which throws on Windows for a name with an unpaired surrogate (an executable renamed so ended in exit 4, "No mapping for the Unicode character", before anything ran), and use `FileSystem::PathToUtf8` (log lines and ImGui's ini name now use forward slashes). Windows regression cases, each proven to fail with the old code: "ProcessContext: an executable named with an unpaired UTF-16 surrogate starts and logs under U+FFFD" and the GPU case "ImGuiLayer: an ini path with an unpaired UTF-16 surrogate does not stop the layer". This closes ADR 0005 decision 24's follow-up (decision 16).
- **A016:** `Sandbox` keeps an atomic count of live VMs (incremented after `lua_newstate`, decremented after `lua_close`), and `ShutdownScriptingRuntime` asserts that it is zero, so §4.1's teardown counters include the Luau VMs; death test "Sandbox: shutting the scripting runtime down while a VM is open asserts", proven to fail without the counter.
- **A017:** `ExitCode.h`'s `Timeout` comment no longer names an engine `--timeout` option (ADR 0024 decision 3).
- **A024:** `Editor --strict` reads every project the editor opens strictly (`EditorContextSpecification::StrictUnknowns`; through `--project`, `project.open`, `project.create` and recovery): unknown keys of the `.eproj`, and unknown keys, fields and components and script field values that do not resolve or match in its scenes and prefabs, are errors instead of warnings; usage text; an unknown key fails with `--strict` and warns without it. Every scene and prefab under `Assets/` must also load strictly: one that does not for any other reason (a structural defect, a file that cannot be read or parsed, a newer `Version`) refuses the project too, naming the file. CI wants every committed scene and prefab to load strictly, so that behaviour is kept and documented (the fix review's C2: the usage text, `EditorContext.h`, §6 and §12.1); "EditorContext: --strict refuses a project whose scene or prefab does not load strictly for any reason, which opens without it" pins it, proven to fail with a check that refuses only files whose error names something unknown. The feature, determinism and export suites pass `--strict` over the committed FeatureTest (the hardening stage's ASan feature step inherits it through `Test.py`), which closes §6's "`--strict` (CI)".
- **A025:** "TypeRegistry: every component field round-trips through automation set and get" iterates the registry over every serializable, non-hidden, non-entity-level component, each in its own scene with its required components, on an entity whose parent has a translation, a rotation and a uniform scale that are not the identity, so a world-space virtual field converts through the parent's space: each virtual field is written through `SetFieldValue` with a seeded value that differs from its current one (float components in [-80, 80] within its metadata, where Euler angles read back as written; a unit quaternion more than 50 degrees away); a writable one reads it back within its conversion's tolerance (1e-3 per component, a quaternion up to its sign), so a setter that ignores its input, drops the parent or mixes degrees and radians fails (the fix review's C1, proven with `SetEulerAngles` returning before it writes; the first version wrote each field's own value back on a parentless entity); a read-only one is refused with `InvalidState` and changes nothing; and 20 seeded random whole-component values go through `CreateMergePatch` and `PatchComponentJson` (the `entity.update` path) and must read back exactly through `GetComponentJson` (values an RFC 7386 patch cannot express are skipped, at least half must be written). The stale comment in `RegistrySuiteTests` goes. §5.4's "automation set/get" is now true for every component added later.
- **A026, A038 (code half):** `ShaderLibrary::Clear`, used only by its own tests, is removed (decision 9); its tests now check that the library keeps what it read at stable addresses.
- **A029 (code half):** `PendingStartTag`, declared but used nowhere, is removed with its existence-only `static_assert`.
- **A065 (code half):** `JsonRpc.h`'s comment on `EngineVersionString` states the v1 constant (decision 6).

#### 13. Fixes of the milestone review

- **M01:** an existing output directory counts as an earlier export only when its `Game.json` (on macOS `<Name>.app/Contents/Resources/Game.json`) loads as a `GameManifest`; any status error other than "not found" refuses the export; `MoveToOutput` checks the directory again right before it replaces it. Regression cases: a directory holding `{}` as `Game.json`, or a lower-case `game.json` with other JSON, is refused and left untouched; `test_export.py` writes a real manifest.
- **M02:** `Export.Company` has one host-independent rule, `FindExportCompanyProblem`: valid UTF-8, no control characters (U+0000 to U+001F, U+007F), neither U+FFFE nor U+FFFF, and at most `ExportSettings::MaxCompanyBytes` (32,000) bytes. Its text part, `FindExportTextProblem`, is also the rule of every string `WriteInfoPlist` writes: `Info.plist` is XML 1.0, whose characters exclude U+FFFE, U+FFFF and every control character but tab, line feed and carriage return, and refusing them all keeps one rule on every host, although the Windows version resource could carry them (the fix review's C7 added U+FFFE and U+FFFF to both checks and corrected the header's claim that the Windows resource cannot carry control characters); `VS_VERSIONINFO` carries up to 32,225 UTF-16 units of it with every other field at its maximum (a 64-byte ASCII `Name`, `<Name>.exe`, version 65535.65535.65535.0), so every host's resources can. The settings validation reports a violation at `/Export/Company`, so `project.setSettings` refuses it and a `.eproj` holding it fails to load (one that loaded before with a control character in `Company` now fails; the repository has none), and `ResolveExecutableResources` reports it as Validation at the same pointer. Because an invalid `Company` can no longer enter an open project, `project.validate` never meets one, which supersedes the decision's "so `project.validate` names it".
- **M03:** `--frames` together with `--export` is a usage error (exit 2). As defense in depth, `EditorApp::AbandonExport` finishes an export still running as `Cancelled`, writes its report and exits 1, on the final frame, a shutdown request and a window close; the last-resort `OnShutdown` path writes the report and logs an error but cannot change the exit code, which `Application::Run` fixes before `OnShutdown`. With `--frames` refused, no command line reaches the defense, so it was shown by hand (usage check disabled: exit 1 with a `Cancelled` report naming `--frames`; both disabled: exit 0, the original defect) and has no automated case.
- **M05:** the export suite checks that each exported report's `Api.Debug` suite row has `breaks == 1` (Release and Dist).
- **M06, M25, M32:** a Lint check, `python-line-length`, enforces CodeStyle's 120-column limit (tabs counted as 4) on every first-party Python file, with a seeded fixture that `Lint.py --self-test` proves; every existing violation is fixed.
- **M07:** the `ExportMethods` M15 field test requires the nine phases `Validate` to `MoveToOutput` in order with non-negative seconds, and `resourcesPatched` true on Windows and macOS, false on Linux.
- **M09:** the `Export.Icon` setting description states the square rule; references regenerated.
- **M10:** with the default `--suite all`, a suite that does not take a requested configuration is reported as not run in it; naming that suite with that configuration stays a usage error.
- **M11:** `MatchesPathGlob`'s wildcards consume whole UTF-8 code points and match only where a code point starts, as `Test.py`'s `glob_regex` does (a byte of an ill-formed sequence counts as one character; literal bytes still compare byte for byte), through the new public `Core/Utf8` helper `GetUtf8SequenceLength`. Twelve non-ASCII vectors (2-, 3- and 4-byte characters under `?`, `*` and `**`, a literal CJK name, case sensitivity) join the shared `Tests/Data/PathGlob/Cases.json`, which the C++ and Python tests both run; nine of them fail with the byte-wise matcher (the fix review's C3: counted by running the shared-cases test against the matcher of the integrated tree, which a trace of it confirms).
- **M12:** the soak's script-heap rule compares floors with floors (`hardening.evaluate_script_heap`). A trough is a sample lower than the one before it, the only kind that shows the live data after a collection. The final floor, the lowest trough of the last quarter (0.75 to 1.00), must not exceed the reference floor, the lowest trough of 0.25 to 0.75, by more than the larger of 128 KiB and 10 % of the reference floor. A window without a collection fails as inconclusive unless its samples bound the growth within that tolerance: without a trough in the last quarter, its highest sample must stay within it of the reference floor; without a trough in either window, the heap only grew from the reference window's first sample on (a scene whose scripts allocate nothing never collects and never grows). The rule integration had chosen compared the last quarter's minimum with the second quarter's maximum, a floor with a ceiling that Luau's 200 % collection goal puts near twice the live heap, and passed stream C's leaking `Callbacks.scene` (heap 0.68 to 20.37 MB). The recorded soaks are committed fixtures (`Tests/Data/Hardening/SoakStreamC.json`, `SoakIntegration.json`): the leaking `Callbacks.scene` fails (floor +2,473,432 bytes against a tolerance of 339,815), the leaking `Input.scene` is inconclusive (no collection in the last quarter, +19.9 MB), and every non-leaking scene passes (at most +32,720 bytes, 2.5 %). The tolerance is four times the largest noise seen and flags a leak of 26 to 40 bytes per tick at today's floors, far below the private memory rule's reach (about 530 bytes per tick). The windows bound the cycles the rule can judge (the fix review's P1): a collection cycle longer than about a quarter of the soak (2,750 ticks of the default 10,000, since a collection after tick 7,250 shows in the last quarter's first sample) can leave the last quarter without a collection, and one longer than half the soak the reference window, so such a scene passes or is inconclusive depending only on its cycle's phase; no recorded non-leaking soak comes near (its troughs lie at most 1,250 ticks apart). Both inconclusive results therefore name `Hardening.py --steps soak --soak-only <subject> --soak-frames <n>`: `--soak-frames N` (at least 10,000 and a multiple of 250) soaks for N ticks, N / 250 editor steps and the Runtime's `--frames N`, with the windows still fractions of the soak and the step's deadline grown in proportion, which confirms such a scene with a longer soak. Architecture §2.3 and §15.1 T6, the `build-and-test` skill and ADR 0024 decision 15 and "The script heap rule" state it.
- **M13:** one soak step deadline (`TIMEOUTS["soak"]`) across the editor and Runtime halves; each call gets the smaller of its own timeout and the time left; at the deadline the step returns a timeout and still writes `Soak.json` and the JUnit report for the subjects that finished.
- **M14:** each fuzz shard runs with `--test-timeout-scale` = `hardening.fuzz_timeout_scale(n)` = max(20, ceil(20 × 19 / n)), so a shard's per-case limit follows its share of the work below the reference run's 19 processes (35 at 11) and never drops below the reference's 20 above it: more shards than physical cores share them through SMT and do not run their smaller share faster (the fix review's P2; the first formula, ceil(20 × 19 / n) alone, fell to 16 at 24 and 12 at 32). Below `hardening.fuzz_minimum_processes` (14: the count whose expected time fits 80 % of the 5,400 s step timeout; 11 would sit at the timeout itself) the step refuses to start with exit 3, naming the available memory or logical CPUs it needs, before it starts anything; `Hardening.py`'s exit code 3 gains that meaning, and `--fuzz-processes` cannot go below the minimum either. The hardening stage therefore needs at least 14 logical CPUs and 14 GiB of available memory (14 shards of 1 GiB) when its fuzz step starts, which Architecture §2.3 and §15.1 T6, `AGENTS.md` and `Hardening.py --help` state (the fix review's P5). ADR 0024 decision 13 and its budgets table state the rule.
- **M15:** `soak_runtime` catches `OSError` like the editor half and records the subject as failed; `Soak.json` and the JUnit report are written from a `finally`.
- **M16:** the two AddressSanitizer tests of `test_hardening.py` patch the runtime lookup to a temporary directory, so no test outside the hardening stage needs Visual Studio's optional ASan component.
- **M17:** "FontImporter: a glyph box more than 2048 pixels on a side is refused before its distance field is allocated" patches a glyph box of `Glyphs.ttf` (1,024 units per em, `PixelSize` 128, `Spread` 1): 2,048 px imports with a 2,048 px atlas rectangle, while 2,049 × 3, 3 × 2,049 and the full ±32,767 box are ImportFailed naming the file and the code point; proven to fail with the guard's limit lifted.
- **M18:** the CFF case rewrites Inter's Name INDEX at the same length with offset size 5, and puts the reserved bytes 31 and 255 into Top DICT operands only the DICT parser reads (`UnderlineThickness`, `UnderlinePosition`); with every other check passing, each fails without its guard. They replace the Global Subr offset-size and Private DICT 255 sub-cases, which failed only on their messages. Each sub-case is now checked on its own (`expectRefused` is not fatal, and a font validation passes is never imported), which also closes guard-proof gap 4. The empty Top DICT INDEX is refused anyway ("has no CharStrings"), so a comment at the check documents it as defense in depth.
- **M20:** the cap case is a one-codebook setup of 16,385 entries × 65,535 dimensions (ordered 15-bit lengths, a type 2 lookup of 1-bit values), whose 2 × entries × dimensions exceeds `INT_MAX` in stb_vorbis's table sizing as the decision's 15,790,321 × 68 would, without allocating 16 MB of lengths; it is the Parse error "at most 1048576 values". Without the cap a plain build fails only on the message (stb's `malloc` of the negative size returns null); the allocation-size-too-big abort that shows the input reaching stb's sizing appears only under ASan, which the hardening stage runs. The lookup1 case appends a 36th codebook (1 entry × 32 dimensions, type 1, two 1-bit multiplicands) to `Tone.ogg`'s setup, rebuilding page 1's lacing and Ogg CRC (the CRC routine is checked against the original page first); without the guard x64 stb_vorbis computes 2 lookup values and decodes the file, so the case fails (40 dimensions would trip stb's own expansion check and make the guard message-only again).
- **M21:** a comment at `CheckVorbisComments` states that its three checks give precise messages and are defense in depth behind the bounds-checked reads and the framing-bit check. With M17, M18 and M20 this closes the four guard-proof gaps of the stream C proofs.
- **M23:** `--test-timeout-scale=<s>` multiplies every per-case limit the Tests binary enforces: the default `--test-timeout` and a limit a case sets with `Test::SetTestCaseTimeLimit(seconds)` as the first statement of its body, both counted from the start of the case; the watchdog is the only per-case limit and ignores `doctest::timeout`, whose post-case check doctest makes itself, unscaled. The eleven decorators are gone: nine cases set their former limits (`SceneEditCommand`, FeatureTest Timing and `Determinism.replay`, the project-settings observation, the validation fixtures and one physics pile at 600 s, one pile at 150 s and one at 450 s, `ScriptTypeChecker` at 120 s), the `TestTimeout` child target sets 0.5 s, and "TestCaseTracker: reports the timeout decorator of the running case" went with `RunningTestCase::TimeoutSeconds`, which nothing else used. "TestTimeout: no test case carries a doctest::timeout decorator" lists the cases in a child, where the timeout listener logs an Error naming each decorated case, and fails on any (doctest calls listeners before its reporter, so a case cannot be failed as it starts). `TestOptions.h`, `TestTimeout.h`, ADR 0024 decision 13 and the `build-and-test` skill state it.
- **M24:** the `ReplayData` fuzz case seeds from its synthetic document and FeatureTest's `.replay` files (at least two required), each used as text and cooked with its expectations compiled as `ReplayImporter` compiles them, the seed chosen by the iteration's `Random`.
- **M27:** the ASan unit step excludes the `Fuzz` suite, which the fuzz step runs as a superset.
- **M28:** the Runtime logs each F1 release ("Runtime: F1 released (stats overlay shown|hidden)"), and the overlay test waits for that line before each release's tick, as it does for the presses. A rare race cannot be shown failing on demand; the wait removes the window.
- **M33:** the Runtime logs "Runtime: stats overlay ready (F1); its ImGui context leaves the OS cursor to the game" right after `ConfigureStatsOverlayContext()`, and the F1 case requires it (another process's OS cursor cannot be read back, and `Window::GetCursorMode` returns a cached value). The line is the case's observable, showing that the windowed Runtime reaches the call; what the function does to the context is covered by the `StatsOverlay` cases. The proof removed the call together with its line, and the case failed (the fix review's C6).
- **M34:** the `--log-level` case, not a Roadmap acceptance name, is renamed "ApplyEngineCommandLine: --log-level sets the output sinks' minimum level and refuses other spellings".

### Documentation

#### 14. The Architecture describes the code again

Every stale text the review kept is amended to the code's behaviour, citing the ADR that decided the change where one exists:

- **§1.2:** "Ogg Opus import" replaces "OGG import" (A015); identity signing and notarization, with ad-hoc signed macOS bundles (A023).
- **§2.1:** the layout tree: no `IBL/` folder, `Resources/{Audio,Licenses}`, `Tests/Source/{Editor,Runtime,Golden}`, `XcodeHeaderSearchPaths.lua`, `Docs/Reference/{Methods.json,Schemas}`, `GameEngineDoc.md`, the demo projects marked with their milestones, CI non-blocking (A020).
- **§2.2:** `Vendor/MikkTSpace` and `UseMikkTSpace()`; the real implementation translation units, compiled with `enablepch "Off"` (A006); `shaders://` from `ENGINE_SHADER_DIRECTORY` and the rule's `--output-dir` (A007).
- **§2.3:** `FetchAssets.py defaults`, `GenerateDocs.py --config`, PreCommit's full static-check list (A021); the DetMath reference check (A002); `Test.py`'s `--suite all` rule (M10) and Lint's `python-line-length` (M06).
- **§3:** the module table rebuilt from `Scripts/ModuleRules.json` and the module directories: the missing `Project` row (A001, the one major finding; ADR 0006 decision 4, ADR 0012 decision 1), every module's contents (the fix review's P10 added `RenderContext`, `TypeList`, `ScriptReference`, `ScriptTime`, `MethodContext` and `JsonReference`, which the first rebuild left out), `ShaderShared`, the private third-party uses (GLFW, stb, Vulkan-Headers, MikkTSpace) and Testing without third-party code (A008); rule 4's importer interfaces in AssetPipeline (A009); rule 5's process-level state (A010).
- **§4:** the §4.1 and §4.2 sketches and the `EngineContextStep` order (A018); `--log-level` in §4.4 (A019); §4.6's error sketch (A012) and allowlist item 1 (A011); §4.10's `shaders://` (A007) and atomic writes (A013); §4.12's DetMath function list and in-house implementation (A014).
- **§5:** the Scene sketch and the session's state hash (A030); entity-level `ID` and `Relationship`, `TypedAssetHandle` and the runtime-only script state (A029, A031); `FieldMeta` and `RegisterComponent` (A031); the `Map` and `Variant` users (A032); prefab instantiation, the four override kinds and override derivation (A033, with §6.2's `Kind`); the play-copy budget (A035); step 0 and frame-phase write detection (A036).
- **§7:** asset references in results (A034); the `AssetManager` and `IAssetImporter` sketches and the scoped failure policy (A037); glTF tangents and buffers (A027).
- **§8:** host image copy (A040), the macOS loader lookup (A044), `WaitForIdle` (A046) and the viewport resize (A039, decision 8) in §8.1; `RenderEnvironment` (A045); no `s3 PointClamp` (A043); the read-only project's `imgui.ini` (A047); `shaders://` in §8.12 (A041).
- **§11:** the removed globals, the require wrapper and `safeenv` (A058); the quoted error texts (A061); `GamepadButton` counted per value (A056); no userdata destructors (A059); `RaycastHit.Body` and `InjectAction`'s optional state (A062); the stack walk and the error dedup key with `jsonPointer` (A053, A060); the CI coverage of type checks (A051); the generated-declaration excerpt (A057); the `FinalStateHash` row and the row fields (A053, A054).
- **§12:** the CLI synopsis, `--strict`, `--export`'s usage error and its earlier-export rule (A055, A024, M03, M01); the `Command` interface, the editor's revision and the history rules (A048, A049; the refuted A050's wording aligned with ADR 0008 decision 34).
- **§13:** the processes a method may start (A066, M31); `TEST_SUITE_INVALID` (A076); the Runtime's `--manifest` and `--paused` (A077); the walkthrough's renderer and save before recording (A067, A068); the demo project paths, Breakout's included (A081).
- **§14:** `Engine.pak`'s contents (A074), `Window.Resizable` (A075) and the macOS rpath, which serves only the dylibs' own references (M08); the output-directory rule (M01); the map check's families, `Utils` rule, ImGuizmo and the ld64 flag (A073).
- **§15:** T0's runners; the lint row and the PreCommit list with the fixture generators, the DetMath reference check and both `FetchAssets.py` checks (A071, A079); `--strict` in the feature, export and determinism rows (A024); GitHub Actions non-blocking (A078); FeatureTest's `Determinism.scene`, tonemapper variants and Editor-only suite (A084).
- **§17, §18, appendices:** the risk rows on remote CI, automation processes and MikkTSpace (A078, A066, A072); `CI.py --milestone` for milestone ends and M16's deliverables (A080, A070); Appendix A's `Lua` namespace and platform layout (A082, A008); Appendix B's vendored-libraries row marked historical (A072); Appendix C's MikkTSpace row resolved and the play-copy row (A027, A072, A035), with the rows of decisions 1, 2, 5, 6, 7, 9 and 11; Appendix D's stray table row (A083).

Breakout stays at `Tests/Projects/Breakout` (Roadmap M16, §2.1, §18 and the ignore rules already put it there), and §13.12 now names it beside `Projects/<Game>/` (A081). The soak's "every demo project" (ADR 0024 decision 15) means the demo games under `Projects/`, which `hardening.soak_projects` scans; the time-boxed dry run is not soaked.

Outside the Architecture: Roadmap M16 (decision 10, and the catch-up of decision 15); `Docs/ReviewChecklist.md`'s automation-security item, aligned with §13.2 (M31); `AGENTS.md`, the `commit-review` and `build-and-test` skills (the DetMath reference check, `--strict` in the suite commands); the fuzz replay command in the `build-and-test` skill and ADR 0024 decision 13, whose case names' commas must be escaped as `\,` or replaced by the `?` wildcard (M26); ADR 0024 decision 5's section reference (M31), decision 11's GNU ld limitation (M30), decision 19's step list (M35) and the Integration record of stream C's engine changes, public API additions and `Fuzz.h` extension (M22).

### Linux and macOS

#### 15. Catch-up items

Remote CI stays non-blocking (ADR 0011). These are fixed in the Linux and macOS catch-up that Roadmap M16 schedules before M16 starts (ADR 0024 decision 20), together with the failures of the M15 milestone commit's Actions runs and a macOS run of an exported bundle (signature, loader lookup):

- **M29:** off Windows, "RuntimeApp: F1 toggles the stats overlay in a windowed run and the game still receives F1" returns after its device probe, because synthesizing a keystroke for another process's window, and the test's Runtime automation connection, are Windows-only. Port the Runtime automation connection to POSIX so the windowed-Runtime overlay path (ImGui enabled, the overlay's GPU objects, a clean exit) runs on Linux and macOS, keeping only the keystroke Windows-only. The toggle, the drawing and the cursor rule are covered there by the device-less `StatsOverlay` cases meanwhile.
- **M30:** verify the map check against a real GCC LTO Dist map. GCC's linker plugin internalizes first-party symbols and GNU ld's map lists only global ones, so the Linux GCC job may miss `FeatureTestRunner`, `ReplayPlayer` or a forbidden symbol; if confirmed, read the executable's ELF symbols beside the map. ADR 0024 decision 11 records the limitation.
- **ADR 0013 decision 25's third limit:** GCC 14, Clang 19 with libstdc++ and Apple Clang still build only in GitHub Actions; the catch-up is where their failures are fixed.

### Closures and dispositions

#### 16. ADR 0005 decision 24's follow-up is closed (A003)

Its open item, `path::u8string()` in `App/ProcessContext.cpp` throwing on Windows for an unpaired surrogate together with the duplicated path helpers, is fixed by decision 12's A003. `Platform/Private/PathsUtf8` stays: besides paths it converts native strings that are not paths (command lines, environment blocks, dialog and resource text), and Core's `NativePath` is private to Core, so merging them would make new public Core API for no defect. The static `AsciiText` helpers (AssetRegistry, PakMount, IAssetImporter, GltfImporter, Framing) are private one-liners and stay. This review disposes of that item only: the decision's other follow-ups belonged to the M5 to M7 contracts, and its optional public `ClockKindToString` stays optional (`FrameLoop.cpp` keeps its private copy).

#### 17. ADR 0007's follow-up is closed (A002, A071)

The DetMath reference header is staleness-checked by the static checks (decision 12), so a hand edit or a stale regeneration of the oracle fails PreCommit and CI's lint stage.

#### 18. ADR 0024 decision 21's items, each disposed of

- §6's `--strict`: fixed (A024, decision 12).
- §4.7's per-frame `FrameArena`: accepted, not built (decision 2).
- §4.13's GPU zones in the profiler timeline: accepted, not exposed (decision 1).
- §8.12, §7.5 and §14.4's shader hot reload: deferred (decision 9).
- §9.6's ground-velocity carry of the character: deferred (decision 11).
- §14.4's Luau compiler in Dist: accepted (decision 5).
- §2.2's MikkTSpace omission: amended (A006, decision 14).
- The stale texts: §1.2 "OGG import" (A015), §7.4, §17 and Appendix C on MikkTSpace (A027, A072) and §4.12's DetMath attribution (A014): amended (decision 14).
- ADR 0005 decision 24's duplicated helpers: closed (decision 16).
- ADR 0007's `GenerateDetMathReference.py --check` outside the static checks: closed (decision 17).
- ADR 0013 decision 25's known limits: (1) a material that leaves the view and returns gets a new binding set: accepted, a cost no Architecture text contradicts; (2) Slang 2026.8 cannot decorate the clip position `Invariant`, so the prepass and forward pipelines agree through `NoContraction` products: accepted while that slangc is pinned (`Scripts/Lib/Toolchain.json`), revisited with the next slangc pin; (3) GCC, Clang and Apple Clang build only in Actions: the catch-up (decision 15); (4) goldens exist for `nvidia-61x` only: accepted, every other device class runs smoke mode (§15.4).
- ADR 0015 decision 27's GPU-suite driver crash: stays an open, tracked follow-up, not an Architecture deviation (no text claims otherwise); the milestone gate's gpu, golden and feature stages show whether it recurs, and the reproduction plan of that decision stands (another driver version or GPU, or the debug heap with GPU object lifetimes logged). It did not recur in the M15 gate run (`CI.py --milestone`: four gpu runs of 260 cases, 25 goldens, the Debug, Release and ASan feature runs).
- ADR 0022 review fix M11 (the 24 golden files without provenance): accepted (decision 7).
- ADR 0023's vendor-library cache follow-up: stays a tracked build follow-up, not decided in v1; no Architecture text depends on it, and its other half, the Debug fuzz iterations, was settled by ADR 0024 decision 14.
- `--validate` and `--timeout` (§12.1, §4.1): settled by ADR 0024 decision 3; nothing further.

### Refuted

#### 19. Four candidates did not hold

- **M04:** `Build.py` catches `ToolchainError` during staging (`main`'s handler), so a missing or unpinned macOS SDK ends in its summary and `--json` report, not a traceback.
- **M19:** the CFF group test stops at its first failing sub-case, but any single guard's regression still fails the case; only simultaneous regressions mask one another, and the suite is red either way.
- **A050:** `SceneEdit`'s Debug check compares each untouched entity's canonical JSON, the exact input of the state hash, which guarantees what §12.3 states; §12.3 now names the mechanism.
- **A064:** Dist has no shader or asset hot reload, as §14.4 says; the real gap is decision 9's, and §14.4 is reworded with it.

## Section dispositions

One row per subsection (ADR 0024 decision 21). "Conforms" means the readers found no deviation; "amended" a stale text corrected by decision 14; "fixed" a code fix of decision 12 or 13; "accepted" and "deferred" refer to the decisions above.

| Section | Disposition |
|---|---|
| §1.1 Goals | conforms |
| §1.2 Non-goals | amended (A015, A023) |
| §1.3 Guiding principles | fixed: principle 2's staleness check (A002) |
| §2.1 Layout | amended (A020) |
| §2.2 Premake | amended (A006, A007) |
| §2.3 Python scripts | amended (A021, M10, M06); `AuditProvenance.py` deferred to M16 (A022, decision 10) |
| §3 Module architecture | amended (A001, A008, A009, A010) |
| §4.1 Application lifecycle | fixed (A016, A017); amended (A018) |
| §4.2 Main loop | amended (A018) |
| §4.3 Platform, window, input | conforms |
| §4.4 Logging | amended (A019) |
| §4.5 Assertions | conforms |
| §4.6 Error-handling policy | fixed (A003); amended (A011, A012) |
| §4.7 Ownership | accepted (A005, decision 2) |
| §4.8 UUIDs | conforms |
| §4.9 Engine events | conforms |
| §4.10 Filesystem and VFS | amended (A007, A013) |
| §4.11 Threading and jobs | conforms |
| §4.12 Time, randomness, DetMath | amended (A014) |
| §4.13 Crash safety and profiling | accepted (A004, decision 1) |
| §5.1 EnTT usage and identity | amended (A030) |
| §5.2 Hierarchy, transforms, active state | amended (A036) |
| §5.3 Component list | amended (A029, A031) |
| §5.4 Reflection registry | fixed (A025); amended (A031, A032) |
| §5.5 Prefabs | amended (A033) |
| §5.6 Scene lifecycle | amended (A035) |
| §5.7 Step order | amended (A036) |
| §6 Serialization (general) | fixed (A024); accepted (A028, decision 3) |
| §6.1 Project | conforms |
| §6.2 Scene | amended (A033) |
| §6.3 to §6.8 | conform |
| §7.1 Types, handles, references | amended (A034) |
| §7.2 AssetManager | amended (A037) |
| §7.3 Registry and integrity | conforms |
| §7.4 Importers | amended (A027, A037) |
| §7.5 Cache, cooking, hot reload | deferred: shader hot reload (A026, decision 9) |
| §7.6 Packaging for export | conforms |
| §8.1 Device and swapchain | accepted (A039, decision 8); amended (A040, A044, A046) |
| §8.2 Frame structure and snapshot | amended (A045) |
| §8.3 Pass list | conforms |
| §8.4 Binding model | amended (A043) |
| §8.5 to §8.10 | conform |
| §8.11 ImGui through NVRHI | amended (A047) |
| §8.12 Shader pipeline | deferred (A038, decision 9); amended (A041) |
| §8.13 Headless rendering | conforms |
| §8.14 GPU resource lifetime | conforms |
| §9.1 to §9.5 | conform |
| §9.6 Character and ball control | deferred (A042, decision 11) |
| §9.7 Determinism tests | conforms |
| §10.1 to §10.4 | conform |
| §11.1 VM ownership and sandbox | amended (A058, A061) |
| §11.2 Script shape and fields | amended (A061) |
| §11.3 Lifecycle callbacks | conforms |
| §11.4 Binding layer | amended (A056, A059, A061) |
| §11.5 Full API surface | amended (A062) |
| §11.6 Creating, destroying, spawning | conforms |
| §11.7 Error reporting | amended (A053, A060) |
| §11.8 Hot reload | conforms |
| §11.9 Static type checking | accepted (A052, decision 4); amended (A051, A057) |
| §11.10 Test scripts and runner | amended (A053, A054) |
| §12.1 Structure | fixed (A024, M01, M03); amended (A055) |
| §12.2 Panels | conforms |
| §12.3 Commands and undo | amended (A048, A049; A050 refuted) |
| §12.4 Play mode, assets, projects | conforms |
| §12.5 Editor design quality | conforms |
| §13.1 Topology | conforms |
| §13.2 Protocol, transport, security | amended (A066, M31) |
| §13.3 Error codes | conforms |
| §13.4 Conventions | conforms (the golden files' provenance: decision 7) |
| §13.5 Method list | conforms |
| §13.6 Lockstep, injection, replays | conforms |
| §13.7 Observability | amended (A076) |
| §13.8 MCP bridge | conforms |
| §13.9 Headless and batch modes | amended (A077) |
| §13.10 Agent-facing documentation | conforms |
| §13.11 Walkthrough | amended (A067, A068) |
| §13.12 Demo acceptance rule | deferred to M16 (A070, decision 10); amended (A081) |
| §14.1 Package format | accepted (A065, decision 6); amended (A074, A075, M08) |
| §14.2 Export pipeline | fixed (M01, M02) |
| §14.3 Runtime executable | conforms |
| §14.4 Dist stripping | accepted (A063, decision 5); amended (A073; A064 refuted) |
| §15.1 Tiers | fixed (A071); T5 deferred to M16 (A070) |
| §15.2 Unit tests per module | conforms |
| §15.3 GPU correctness tests | conforms |
| §15.4 Golden images | accepted (A069, decision 7) |
| §15.5 FeatureTest project | amended (A084) |
| §15.6 Coverage gates | conforms |
| §15.7 Automation and game tests | conforms |
| §15.8 CI entry point | fixed (A071, A024); amended (A078, A079); games deferred to M16 (A070) |
| §15.9 Commit gate and code review | amended (A079) |
| §16 Cross-platform concerns | conforms (Linux and macOS verification: decision 15) |
| §17 Risks and mitigations | amended (A066, A072, A078) |
| §18 Milestone plan | amended (A080, A070) |
| Appendix A | amended (A082, A008) |
| Appendix B | amended (A072) |
| Appendix C | amended (A027, A072, A035); rows added by decisions 1, 2, 5, 6, 7, 9 and 11 |
| Appendix D | amended (A083) |

## Consequences

- The Architecture is authoritative again for every section; a reader no longer needs an earlier ADR's amendment list to know what the code does.
- Appendix C holds every v1 deferral the review found, each with the default in force, so a later milestone can pick one up deliberately.
- An ADR that requests Architecture amendments applies them in its own change, as AGENTS.md's deviation rule requires; the completion review is not a substitute.
- The single `CI.py --milestone` run gates the milestone commit after these fixes (ADR 0023 decision 2; ADR 0024 decision 19).
