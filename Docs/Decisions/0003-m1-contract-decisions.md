# 0003 — M1 contract decisions

- **Status:** proposed by the M1 contract task and revised after its two reviews (API design; style and build). The M1 integration task resolved decision 10 and added decisions 21 to 26; the M1 review added decision 27 and revised decisions 8, 14, 21, 22, 24, 25 and 26. The docs owner applies the amendments listed at the end.
- **Date:** 2026-10-05
- **Context:** The M1 contract task (Roadmap rule 3) froze the public headers of every M1 deliverable in `Engine/Source/Engine/Core/` and `Tests/Source/Support/`. Writing complete headers exposed places where the Architecture and Roadmap are silent, contradict each other, or schedule a dependency after its first user. `AGENTS.md` ("Deviations") requires a record of each departure.

## Decisions

### 1. Death-test children are spawned by a Tests helper until `Platform::Process` exists

Architecture §4.5 has the parent of a death test spawn the child "through `Platform::Process`", but `Platform::Process` is an M2 deliverable while Roadmap M1 requires the death test `Core/AssertFires` to pass.

- **What M1 does:** `Tests/Source/Support/ChildProcess.h` declares `Test::RunChildProcess` (arguments passed verbatim, captured output, timeout). Its implementation, `Tests/Source/Support/ChildProcess.cpp`, is the only Tests file with OS process code, guarded by `ENGINE_PLATFORM_*`. `Scripts/ModuleRules.json` grants OS headers to that one file through the `TestsChildProcess` project rule.
- **Executable path:** `Test::GetCurrentExecutablePath` (same file) asks the OS for the running binary (`GetModuleFileNameW`, `/proc/self/exe`, `_NSGetExecutablePath`). `TestOptions::ExecutablePath` uses it and falls back to `argv[0]` only when the query fails, because `argv[0]` may be a bare name without `.exe` or anything the parent chose.
- **M2:** re-implements `RunChildProcess` and `GetCurrentExecutablePath` on `Platform` behind the same declarations, deletes the OS code and removes the `TestsChildProcess` rule.

### 2. `FatalError` lives in Core

Architecture §4.6 names `FatalError(kind, message)` and has the `Core/Jobs/JobSystem.cpp` worker catch-all call it, and §4.5 ends every failed assertion in the crash path with exit code 4. Core may include nothing above it, so the function is declared in `Core/FatalError.h`. The effects that need higher layers (editor autosave, crash report, message box) are injected by `ProcessContext` (M2) through `SetFatalErrorHandler`. The exit codes 3 and 4 are duplicated as `FatalInitFailedExitCode` and `FatalCrashExitCode`; `App/ExitCode.h` (M2) must keep its `InitFailed` and `Crash` values equal to them.

`FatalError` flushes the log sinks and the C stdio streams (`std::fflush(nullptr)`) before `std::_Exit`, which flushes nothing itself. Without that, doctest's report on a redirected (fully buffered) stdout would be lost whenever an assert ends a test run. The Tests assert handler also appends `[test case: <name>]` to its Critical line on stderr, so §4.5's "doctest's crash reporting names the running test case" holds even without the report.

### 3. Core files beyond the Roadmap M1 list

Each one is needed by a frozen header and owned by the stream of its users:

- `Core/FatalError.h|.cpp` (decision 2; stream A);
- `Core/Buffer.h` (`using Buffer = std::vector<std::byte>` and byte/text views, the return type of `ReadFile`; stream C);
- `Core/Utf8.h|.cpp` (UTF-8 validation shared by `ReadText`, `VfsPath`, `BinaryReader::ReadString` and the JSON reader and writer; stream C);
- `Core/Json/Json.h` (the `Json` tree alias and `JsonType`, shared by the reader and the writer; stream C);
- `Core/UniqueFunction.h` (a move-only callable for jobs and main-thread tasks, because `std::move_only_function` is missing from the libc++ of the minimum Xcode; stream D);
- `ENGINE_CONCAT` in `Core/Base.h`, for the per-line identifiers of `ENGINE_TRY_ASSIGN`, `ENGINE_PROFILE_SCOPE` and `ENGINE_DEATH_TEST`.

### 4. The engine's JSON tree is `nlohmann::ordered_json`

The Architecture names nlohmann but not the tree type. `Engine::Json` is `nlohmann::ordered_json`: object members keep document order, so data the engine does not understand (unknown components, `Variant` values, §6) is written back verbatim, and no iteration order depends on a hash. Canonical key order for reflected types is still decided by the writer's caller (registry order); `Map` fields are written in `std::map` order.

### 5. Atomic writes use `std::filesystem` in Core

Architecture §4.10 describes the atomic write as "temp file → flush → `ReplaceFileW` (Windows) / `rename` (POSIX), keeping one `.bak`". Core may not include OS headers (§3, `ModuleRules.json`), and `FileSystem` is a Core deliverable.

- **What M1 does:** temporary file in the target's directory, write, flush and close, copy the old file to `<name>.bak`, then `std::filesystem::rename` over the target. That rename is `rename(2)` on POSIX and `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)` in the MSVC STL: an atomic replace on one volume. At every instant the target holds the complete old or the complete new content, which is the guarantee §4.10 states ("a crash mid-save never corrupts a file").
- **Not provided:** `fsync`/`FlushFileBuffers` durability across a power loss. Adding it needs OS calls, so it would come through a Platform hook if ever required.
- **Testability:** `AtomicWriteOptions::InjectFailure` fails a chosen step, for the Roadmap test "FileSystem: atomic write survives an injected failure".

### 6. The profiler's buffers are process-level state

`ENGINE_PROFILE_SCOPE("Name")` (§4.13) takes no context argument, so each thread records into a thread-local ring registered with a process-wide `Profiler`, initialized by `ProcessContext` or the Tests main like the logger registry. §3 rule 5 does not list it yet.

- **Ordering rule:** as with `Log`, `Initialize` and `Shutdown` run only while no other thread records. Lock-free recording into thread-local rings cannot be combined with freeing those rings concurrently without epoch-based reclamation, which a diagnostic profiler does not need.
- **Re-initialization:** each `Initialize` starts a new generation, and a thread whose cached ring belongs to an earlier generation registers a new ring. Tests cycle `Shutdown` and `Initialize` on threads that outlive the cycle.
- **Dist:** scopes record nothing in Dist, which has neither automation nor `stats.get`. The Dist macro still type-checks its argument without evaluating it, like a compiled-out assert, so a name held in a variable never becomes an unused variable in Dist only.

### 7. Logging details

- The console sink writes to stderr, so stdout stays free for machine-readable output (`--json`), and death-test parents find assert messages on stderr (§4.5).
- Log listeners (`Log::AddListener`) are the mechanism behind `Test::ExpectLog`: a listener fails the running test case as soon as an undeclared Error or Critical entry is stored.
- Test cases that exist only to be run in a child process by another test, such as the watchdog target and the recording-assert-handler target, are permanently skipped and tagged with the `ChildTargets` suite (`Test::ChildTargetSuite`). They hang or end the process by design, so a manual `Tests --no-skip` run passes `--test-suite-exclude=ChildTargets`.

### 8. Scheduler and clock details

- `FixedStepScheduler::Advance` is specified exactly in `FixedStepScheduler.h` (frame clamp, step cap, a 1e-9 s step tolerance, dropped time in simulation seconds), because the Roadmap table test needs exact expectations.
- `FixedStepScheduler::StepExactly(n)` runs exactly `n` steps with `Alpha = 1`. It serves `ManualClock` ("every frame contains exactly one fixed step and Alpha is defined as 1", §4.2) and lockstep `play.step`.
- `FrameSteps` gains `FirstTick`, the tick of the frame's first step.
- `FrameLoopConfig::MaxFixedHz = 100000` bounds `FixedHz` (asserted by the scheduler). Above about 1 GHz, `FixedDelta` would fall below the step tolerance, and a frame with an empty accumulator would run steps. At the bound, `FixedDelta` (1e-5 s) is 10,000 times the tolerance. The M3 project loader validates `Simulation.FixedHz` against `[1, MaxFixedHz]`.

### 9. `Engine/Config` is an include root, not a C++ root, in `ModuleRules.json`

Decision 2 of `0002-m0-deviations.md` asks M1 for "its `ModuleRules.json` entry". `Engine/Config` is added to `IncludeRoots`, so a first-party file that includes `<entt/ext/config.h>` directly is reported. It is not added to `CxxRoots`: EnTT dictates the lower-case path `entt/ext/config.h`, which the PascalCase file-name check would reject. The header is compiled by every translation unit that includes EnTT (the `Core/EnttAssertRoutesToEngine` death test among them).

### 10. Values a contract cannot contain

The committed DetMath output hash ("DetMath: output hash over 1,000,000 seeded inputs matches the committed value") can only be recorded from the first implementation. The skipped test fixes the input generation and leaves `CommittedHash` at 0; stream B records the value when it un-skips the test, and it never changes afterwards.

**Resolved:** the committed hash is `0x1f03563cdf5ec2cf` (`Tests/Source/Engine/Core/DetMathTests.cpp`). The test passes with that value in Debug and Release, and in the clang-cl Release build of the `portability` stage.

### 11. `Error::GetMessageText` instead of `GetMessage`

Architecture §4.6 names the accessor `GetMessage()`, but `<windows.h>` defines `GetMessage` as a macro (`GetMessageW`).

- **Why it cannot stay:** every Engine and Tests translation unit on Windows defines `VK_USE_PLATFORM_WIN32_KHR` (`UseNVRHI` in `Dependencies.lua`), and `vulkan.h` then includes `<windows.h>`. CodeStyle §4.4 puts the file's own header and repository headers before third-party and OS headers, so any Graphics, Renderer, ImGui or Platform/Windows file would see the macro after `Error.h` and fail to compile `error.GetMessage()`. Including OS headers first would break the include order and give `Error` different member names in different translation units.
- **Decision:** the accessor is `GetMessageText()`, renamed while nothing depends on it. No other public Core name collides with a Windows macro.

### 12. Errors carry issues, and the root JSON pointer is a location

- **Issues:** §13.3 automation errors carry `issues[]`, each `{pointer, message, hint?, suggestions?}`, and M3 validation reports every bad field of one request at once. Automation handlers return `Result<T>` (§4.6 boundary), so `Error` carries them: `ErrorIssue {JsonPointer, Message, Hint, Suggestions}`, `WithIssue`, `WithIssues` (attaches a collected vector, which avoids re-assigning an error to itself in a loop) and `GetIssues`. `ToString` appends each issue after " | ".
- **Root pointer:** RFC 6901 and `JsonReader` use `""` for the document root, so `ErrorLocation::JsonPointer` is a `std::optional<std::string>`: no value means "not set", and `""` is the root. `ToString` writes the root as `(root)`. A root-level error, such as "missing required field 'Format'", is therefore located like every other.

### 13. The VFS mount table is locked, and dry runs require idle project:// jobs

§13.4 dry runs swap `project://` for an `OverlayMount` (Unmount, wrap, Mount) on the main thread while JobSystem workers may be reading `project://` for import, decode, cook or file-watcher polling (§4.11).

- **Locking:** a reader-writer lock guards the mount table. Each forwarding call holds it shared until its mount call returns, and `Mount`/`Unmount` hold it exclusively. `Unmount` therefore waits for calls in flight, no call ever uses a removed mount, and the public signatures stay the same. Mounts never call back into the VFS.
- **What a job would see:** the swap is visible to every thread. A job reading during the dry run would see overlay content (and could cook it into `cache://`, breaking "nothing touches the disk"), and a job writing would write into the overlay and lose its write.
- **Rule for M4:** the EditorCore dry-run path submits no `project://` job during the call and waits for the running ones (`JobSystem::WaitIdle`) before the swap.
- **Rejected alternative:** a separate dry-run `VirtualFileSystem` view would need mounts shared between two file systems, that is `Ref<IMount>` for mutable objects (§4.7 reserves `Ref` for shared immutable data) or borrowed mounts with documented lifetimes.

### 14. The case policy covers new names and case-only renames

The §4.10 case policy only covered reads and the existing directories of a write. Writing `Assets/level1.scene` next to `Assets/Level1.scene` would create a second file on Linux and `MemoryMount`, and silently overwrite `Level1.scene` on Windows and macOS.

- **New names:** every mount now rejects a write, `CreateDirectories` or `Move` destination whose final component matches an existing entry only when ASCII case is ignored. The error is Validation "case mismatch", it names the stored spelling and nothing changes. No directory ever holds two names that differ only in case.
- **Case-only renames:** a `Move` whose destination differs from its source only in case renames the entry in place, on every host. That is how a case mismatch is fixed. `FileSystem::Move` documents the same rule.
- **Other host aliases:** a host can resolve a name that no directory entry spells, even ignoring ASCII case, to an existing entry: NTFS and APFS fold the case of non-ASCII letters, APFS and HFS+ ignore Unicode normalization, and Windows resolves 8.3 short names. `NativeDirectoryMount` asks the host about every new name that the case policy found free. When the host resolves it to an existing entry, the write, `CreateDirectories` or `Move` is Validation naming that entry, and nothing changes. This cannot behave the same on every host: Linux and `MemoryMount` do not alias these names and create a new entry. What holds everywhere is that no mount silently replaces an entry spelled differently.

### 15. File streams read snapshots

`IFileStream` promised that a file rewritten while open reads "either content". On Windows, a stream opened through the standard C++ streams has no `FILE_SHARE_DELETE`, so the atomic replace behind `WriteFileAtomic` (and `Remove`, `Move`) would fail while it is open, on Windows only.

- **Decision:** native, memory and overlay mounts stream a snapshot held in memory, taken at `Open`. A stream never keeps a host file open, rewriting, moving or removing the file succeeds on every host, and the stream reads the content of the moment it was opened.
- **Exception:** a read-only `PakMount` (M6), whose archive never changes while mounted, may read the archive directly.
- **Lifetime:** a stream does not depend on its mount and stays valid after the mount is unmounted, which the locking of decision 13 relies on.

### 16. `.bak` backups are a per-mount choice

`NativeDirectoryMount` takes `AtomicWriteOptions`. `project://` keeps the default `KeepBackup = true` (§4.10 "keeping one `.bak`"). `cache://`, `enginecache://` and `user://` pass `false`: they hold regenerable or per-user data, and a backup of every cooked file would double their I/O and disk use. The same options let a test inject a failure into every write of a mount.

### 17. JSON headers have a minimum version, and nesting is bounded

- **Minimum version:** `JsonReader::ReadFormatHeader(format, minimumVersion, supportedVersion)`. Versions below the minimum are Validation, newer ones UnsupportedVersion. A format whose oldest fixtures predate version 1 passes 0, so Roadmap M3 "Migrations: v0 fixture upgrades to v1" reads its fixture through the shared header check.
- **Depth:** `MaxJsonDepth = 512` (`Json.h`). `JsonReader::Parse` rejects deeper input with a located Parse error. `JsonWriter::WriteJson` asserts the bound, because copying, destroying and writing a tree recurse once per level. Without it, a document of a million `[` would overflow the stack, which breaks §15.2's never-crash, always-Result oracle.

### 18. DetMath provides Sinh, Cosh, Tanh and Log10

§4.12 rebinds Luau's `math.sinh`, `cosh`, `tanh` and `log10` to DetMath but lists only `Sin` through `Pow`. All four are added for `float` and `double`. They are covered by the accuracy and committed-hash tests, and `Log10` is exact at every power of ten the type represents exactly. Composing them in Scripting would be inaccurate: `Log(1000) / Log(10)` is 2.9999999999999996, which breaks digit counting with `floor(math.log10(n))`.

### 19. Editor UUID entropy comes from outside Core

§4.8 seeds the editor's generator from OS CSPRNG bytes, but Core is on the simulation path, where `std::random_device` is banned (`AGENTS.md`), and it may not call the OS. `UUIDGenerator::CreateRandom(const Random::State&)` takes the 256-bit state. Platform (M2) provides the OS bytes, and EditorCore passes them in.

### 20. Determinism details

- **`Random::RangeDouble`:** specified as an exact formula that cannot overflow (`half = 0.5 * max - 0.5 * min`, `r = (min + half * u) + half * u`), clamped below `max`. Every call consumes exactly one draw, `min == max` included. The naive `min + (max - min) * u` can round up to `max` and overflows to Inf or NaN for finite bounds such as ±1e308.
- **`BinaryWriter::WriteArray`:** copies object representations, so its element types must have no padding. Each cooked struct pins its layout with a `static_assert` on its size.

### 21. Implementation choices recorded at M1 integration

The five M1 streams had to choose behaviour the contract left open. These choices are now documented in the frozen headers:

- **Log file sink.** It is a custom spdlog sink built on the standard streams. spdlog's `rotating_file_sink` reports open and rename failures by throwing, and `Log.cpp` may not catch (§4.6). The rotated files are `<exe>.log`, then `<exe>.1.log` to `<exe>.4.log`, 5 files of 10 MB each.
  - `Log::Initialize` creates the log directory and returns Io when it cannot open the file.
  - Later write and rotation failures cannot be logged, because the sink holds its own lock. They are absorbed: the file is truncated to stay within its limit, and reopening is retried on the next entry. The entries are still in the ring buffer and on the console.
- **Log listeners.** Listeners run while the listener registry is held shared, so `RemoveListener` waits for a listener that is still running. Calling `AddListener` or `RemoveListener` from a listener is asserted. An entry logged from inside a listener, such as an assertion report, is stored but not passed to the listeners again.
- **Static destruction.** The objects the log creates lazily are never destroyed (`Core/Private/Immortal.h`): the fallback logger, the listener registry and the empty ring buffer of an uninitialized log. Neither is `FatalError`'s mutex, because libc++ cannot lock a destroyed `std::mutex`. The fallback logger writes to stderr through its own sink, because spdlog's stderr sinks lock spdlog's function-local console mutex. Logging, `Log::Flush` and `FatalError` therefore keep working in static destructors after `Log::Shutdown`. A process that exits without `Log::Shutdown` and logs from a static destructor still reaches spdlog's console sink, so `ProcessContext` (M2) shuts the log down before the process exits, as the Tests main does.
- **Breaking into the debugger.** `DefaultAssertHandler` logs the assertion and exits with code 4 but does not break into an attached debugger. Core may not include OS headers, and C++26 `<debugging>` is in none of the M1 standard libraries.
  - M2's `ProcessContext` adds the break with no header change: it installs a `FatalErrorHandler` that breaks for `FatalErrorKind::Assert` when Platform reports an attached debugger, and then writes the crash report.
  - That handler runs after the assertion is logged and before exit 4, which is the order §4.5 gives.
- **DetMath.** The implementation is not derived from Jolt's `Trigonometry.h`. It uses tables, short polynomials and double-double arithmetic, with Cody-Waite reduction and an exact integer Payne-Hanek reduction for huge arguments.
  - It uses only IEEE-exact operations, plus exact integer arithmetic and integer/floating conversions.
  - Measured against exact references, it is within 0.5005 ULP everywhere except exp results in the subnormal range, which are within 0.72 ULP. The contract still promises 2 ULP.
  - No `LICENSES.md` attribution is needed.
- **JSON.** `JsonReader::Parse` treats input as follows:
  - An integer literal outside the int64 and uint64 range is a located Parse error. nlohmann would turn it into a float, which contradicts `Json.h`.
  - A leading UTF-8 byte order mark is accepted (RFC 8259 allows it) and dropped, so loading and then saving removes it.
  - A syntax error is located where nlohmann stopped reading, which can be one token after the actual problem.
  - It uses `json::parse(first, last, nullptr, false)` plus `is_discarded()`, and no SAX handler.
- **Move.** Moving the mount root, or moving a directory into its own subtree, is InvalidArgument in `FileSystem::Move` and in every mount.
- **Jobs, events and the profiler.**
  - `JobSystem::WaitIdle`, or destroying the `JobSystem`, from inside one of its own jobs would never return, so it is an `ENGINE_CORE_VERIFY` failure in every configuration.
  - A worker thread that cannot start is `FatalError(InitFailed)`. The catch is in the allowlisted `JobSystem.cpp`.
  - A ring buffer cursor past the end (for `RingBufferSink` and `EventLog`) returns nothing and resumes at `GetNextSeq()`.
  - `Profiler::RecordZone` asserts its arguments. `SubmitGpuZones` drops inverted GPU zones, which are driver data.
  - Each thread's profiler ring outlives the thread: the 16 most recently exited threads stay collectable until Shutdown.

### 22. Members added to frozen headers

Each stream added only private members (state and helpers); no public declaration changed:

- `RingBufferSink.h`: the entry storage and its mutex.
- `Hash.h`: the streaming state of `XXH64Hasher`.
- `UUIDGenerator.h`: `m_SessionSeed`.
- `Clock.h`: the previous sample of `SystemClock`.
- `FixedStepScheduler.h`: `m_Accumulator`.
- `EventLog.h`: the ring, the main-thread ID and two private helpers (`GetHeld`, `GetOldestSeq`).
- `Jobs/JobSystem.h`: the running-thread list, the stop flag and two helpers, and `WorkerMain` takes the worker index.
- `Jobs/MainThreadQueue.h`: `m_IsDraining`.
- `Profiler.h`: `ProfileScope::m_Generation`.

The integration task, as contract owner, reviewed and accepted them. The M1 review added one public declaration, `FrameLoopConfig::MaxFixedHz` (decision 8).

### 23. `NativeDirectoryMount` holds a reader-writer lock

`IMount` requires each call to be atomic with respect to the other calls on the same mount. The case check and the change in `NativeDirectoryMount` are separate host operations. Without a lock, two threads writing `level1.scene` and `Level1.scene` at the same time could both pass the case check.

- The mount now has a private `mutable std::shared_mutex`. Reads hold it shared and mutations hold it exclusively.
- A process-wide mutex would have broken §3 rule 5.
- Other processes, and other mounts of the same directory, are not covered.
- The test "NativeDirectoryMount: concurrent writes of two spellings of one name create exactly one file" covers it. It fails without the lock.

### 24. Additional private files

They extend decision 3 and are owned by the streams of their users:

- `Core/Private/NativePath.h|.cpp`: UTF-8 to `std::filesystem::path` and back, without throwing.
- `Core/Private/PathText.h|.cpp`: the case-policy text helpers for relative paths.
- `Core/Private/AsciiText.h|.cpp`: `ToAsciiLower` and `EqualsIgnoreAsciiCase`, shared by the case policy and by the name parsers of `RingBufferSink` (log levels and channels) and `EventLog` (event types).
- `Core/Private/Immortal.h`: storage whose destructor never runs, for the process-level state that `Log` and `FatalError` still use during static destruction (decision 21).
- `Core/Mounts/Private/SnapshotFileStream.h|.cpp`: the snapshot stream of decision 15.
- `Tests/Source/Support/TestCaseTracker.h|.cpp`: the doctest listener that tracks the running test case for other threads, and the shared listener base class of `ExpectLog` and `TestTimeout`.

### 25. Tests main and meta-tests

- **Log start-up.** The Tests main exits with code 3 (InitFailed) when `Log::Initialize` fails. Without the log, `ExpectLog` would see no entries, and undeclared errors would pass unnoticed.
- **No log file yet.** The Tests main starts the log without a file sink. §4.4 gives the Tests a rotating `<UserData>/<AppName>/Logs/<exe>.log`, but `<UserData>` comes from Platform `Paths` (M2). The Tests binary's file sink arrives with it.
- **ExpectLog meta-tests.** The meta-tests that prove an undeclared or missing entry fails a test case run their `ChildTargets` targets in a child process, rather than using `doctest::should_fail`. A `should_fail` case still writes `<failure>` elements into the JUnit report that `Test.py --junit` reads.
- **Death tests.** Asserted preconditions are covered by death tests, among them `Random`, `ManualClock`, `FixedStepScheduler`, `UUIDGenerator`, `JobSystem`, `MainThreadQueue`, `EventLog`, `HandlePool`, `UniqueFunction` and `Profiler`. The only permanently skipped cases are the `ChildTargets` of decision 7.

### 26. Third-party override names in the naming rules

Two third-party virtual interfaces dictate snake_case names:

- spdlog's `base_sink` (`sink_it_`, `flush_`), overridden by the log sinks;
- doctest's `IReporter` (`report_query` to `test_case_skipped`), overridden by the Tests listeners that §4.4 requires.

clang-tidy skips overrides. The regex fallback of `Scripts/Lint.py`, which runs wherever clang-tidy is missing, now skips a member function declared `override` or `final` too. It cannot see the base class, so it relies on the virt-specifier, which only an in-class declaration carries: an override with such a name is defined inside its class. The names are not listed in `.clang-tidy`'s `MethodIgnoredRegexp`, so a first-party function that overrides nothing and is named like them is still reported in both modes. The lint fixture `OverrideNaming` checks both halves in both modes.

### 27. The directory-wide load-then-save test arrives with the first authored files

§6 asserts load → save byte-identity "for every authored file under `Projects/` and `Tests/Data`". M1 has no such file: `Projects/` does not exist and `Tests/Data` holds only the lint and build-configuration fixtures. M1's "JsonWriter: load then save is byte-identical" round-trips an embedded canonical document.

- **Deferred:** the test that enumerates `Projects/` and `Tests/Data`, skipping `.jsonl` files and project `Automation/` folders, lands with the first authored fixtures in M3. That milestone also decides how the Tests binary finds the repository's data directories.
- **Why not now:** the Tests binary has no way yet to locate `Tests/Data`. Choosing one (a premake define, a path relative to the executable or a command-line option) belongs to the milestone that first reads fixtures.

## Requested amendments (docs owner)

- **Architecture §3 rule 5:** add the profiler's per-thread buffers to the process-level state, with the `Log`-style ordering rule (decision 6).
- **Architecture §4.2:** `StepExactly` and `FrameSteps::FirstTick` (decision 8).
- **Architecture §4.4:** the rotated log files are `<exe>.1.log` to `<exe>.4.log`, written by a custom sink that never throws, and listeners follow the rules of decision 21.
- **Architecture §4.5:**
  - Death-test children are spawned by `Test::RunChildProcess` (decision 1).
  - The console sink writes to stderr (decision 7).
  - The Tests handler names the running test case on stderr, and `FatalError` flushes C stdio (decision 2).
- **Architecture §4.6:**
  - `FatalError` is declared in `Core/FatalError.h` with an injected handler (decision 2).
  - The accessor is `GetMessageText()` (decision 11).
  - `ErrorLocation::JsonPointer` is optional, with `""` meaning the root; `Error` carries `ErrorIssue`s (decision 12).
- **Architecture §4.8:** the editor's generator takes a xoshiro state filled from OS bytes by Platform (decision 19).
- **Architecture §4.10:**
  - The atomic write uses `std::filesystem::rename` and makes no power-loss durability claim (decision 5).
  - The mount table is locked, and the dry-run swap requires idle `project://` jobs (decision 13).
  - The case policy covers new names and case-only renames, and a new name that the host resolves to an existing entry is Validation (decision 14).
  - Streams read snapshots (decision 15).
  - Only `project://` keeps `.bak` files (decision 16).
  - `NativeDirectoryMount` serializes its calls with a reader-writer lock (decision 23).
  - Moving the mount root, or moving a directory into its own subtree, is InvalidArgument (decision 21).
- **Architecture §4.12:**
  - The DetMath list gains `Sinh`, `Cosh`, `Tanh` and `Log10` (decision 18).
  - Drop the "style of Jolt's `Trigonometry.h` ... attributed in `LICENSES.md`" wording, and add exact integer arithmetic and integer/floating conversions to the allowed operations (decision 21).
- **Architecture §6 or §4:**
  - `Engine::Json` is `nlohmann::ordered_json` (decision 4).
  - `MaxJsonDepth` and the minimum header version (decision 17).
- **Architecture §6.1:** `Simulation.FixedHz` is in `[1, 100000]` (`FrameLoopConfig::MaxFixedHz`, decision 8).
- **Architecture §13.4:** dry runs wait for `project://` jobs and submit none during the call (decision 13).
- **Architecture §15.2:** the Tests main also parses `--test-timeout=<seconds>` (the per-case limit for cases without `doctest::timeout`, default 120 s, exit code 5). The `ChildTargets` suite holds the cases that only run as child processes, and a manual `--no-skip` run excludes it (decision 7). The Tests log file sink arrives with M2 (decision 25).
- **Roadmap M1 deliverables:** the files of decisions 3 and 24.
- **Roadmap M3 acceptance:** every authored file under `Projects/` and `Tests/Data` (`.jsonl` and `Automation/` exempt) is byte-identical after `JsonReader::Parse` and `JsonWriter::Write`, and the Tests binary has a way to locate those directories (decision 27).
- **Roadmap M2:**
  - `Process` replaces the implementation of `Test::RunChildProcess` and `Test::GetCurrentExecutablePath`, and the `TestsChildProcess` lint rule goes away (decision 1).
  - `App/ExitCode.h` matches the Core exit-code constants (decision 2).
  - `ProcessContext` breaks into an attached debugger from its `FatalErrorHandler` for `FatalErrorKind::Assert` (decision 21).
