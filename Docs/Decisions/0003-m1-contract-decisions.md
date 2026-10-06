# 0003 — M1 contract decisions

- **Status:** proposed by the M1 contract task and revised after its two reviews (API design; style and build); the docs owner applies the amendments listed at the end.
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

### 9. `Engine/Config` is an include root, not a C++ root, in `ModuleRules.json`

Decision 2 of `0002-m0-deviations.md` asks M1 for "its `ModuleRules.json` entry". `Engine/Config` is added to `IncludeRoots`, so a first-party file that includes `<entt/ext/config.h>` directly is reported. It is not added to `CxxRoots`: EnTT dictates the lower-case path `entt/ext/config.h`, which the PascalCase file-name check would reject. The header is compiled by every translation unit that includes EnTT (the `Core/EnttAssertRoutesToEngine` death test among them).

### 10. Values a contract cannot contain

The committed DetMath output hash ("DetMath: output hash over 1,000,000 seeded inputs matches the committed value") can only be recorded from the first implementation. The skipped test fixes the input generation and leaves `CommittedHash` at 0; stream B records the value when it un-skips the test, and it never changes afterwards.

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

## Requested amendments (docs owner)

- **Architecture §3 rule 5:** add the profiler's per-thread buffers to the process-level state, with the `Log`-style ordering rule (decision 6).
- **Architecture §4.2:** `StepExactly` and `FrameSteps::FirstTick` (decision 8).
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
  - The case policy covers new names and case-only renames (decision 14).
  - Streams read snapshots (decision 15).
  - Only `project://` keeps `.bak` files (decision 16).
- **Architecture §4.12:** the DetMath list gains `Sinh`, `Cosh`, `Tanh` and `Log10` (decision 18).
- **Architecture §6 or §4:**
  - `Engine::Json` is `nlohmann::ordered_json` (decision 4).
  - `MaxJsonDepth` and the minimum header version (decision 17).
- **Architecture §13.4:** dry runs wait for `project://` jobs and submit none during the call (decision 13).
- **Roadmap M1 deliverables:** the files of decision 3.
- **Roadmap M2:**
  - `Process` replaces the implementation of `Test::RunChildProcess` and `Test::GetCurrentExecutablePath`, and the `TestsChildProcess` lint rule goes away (decision 1).
  - `App/ExitCode.h` matches the Core exit-code constants (decision 2).
