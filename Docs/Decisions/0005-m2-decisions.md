# 0005 — M2 contract decisions

- **Status:** proposed by the M2 contract task. The M2 integration task and review accept, revise or extend it; the docs owner applies the amendments listed at the end.
- **Date:** 2026-10-06
- **Context:** The M2 contract task (Roadmap rule 3) froze the public headers of every M2 deliverable in `Engine/Source/Engine/Platform/`, `Engine/Source/Engine/App/` and `Tests/Source/Support/`, and wired the Editor and Runtime mains to an empty `Application` subclass. Writing complete headers exposed places where the Architecture and Roadmap are silent, and a few where they cannot be followed literally. `AGENTS.md` ("Deviations") requires a record of each.

## Decisions

### 1. OS code lives in `Platform/Windows/` and `Platform/Posix/`, next to a host-independent `.cpp`

`Scripts/ModuleRules.json` grants OS headers to Platform's `Windows/`, `Linux/`, `MacOS/` and `Posix/` subfolders only, so a `Platform/<Unit>.cpp` at the module root cannot call `LockFileEx`, `flock` or `CreateProcessW`. The Roadmap lists `Process.h|.cpp`, `Paths.h|.cpp` and `ProjectLock.h|.cpp` as single pairs.

- **Layout:** each unit with OS calls has `Platform/<Unit>.cpp` for the host-independent part and `Platform/Windows/<Unit>Windows.cpp` plus `Platform/Posix/<Unit>Posix.cpp` for the OS part: Process (`Run` on top of `Spawn`/`Wait`/`Kill`), Paths (name validation and layout; `GetUserDataRoot` per OS), ProjectLock (`ReadHolderPid`; the lock per OS), CrashHandler (breadcrumbs and report text; handlers per OS), SecureRandom (`GenerateState`; `Fill` per OS) and Socket (OS files only, as the Roadmap says).
- **Linux and macOS** share the `Posix/` file of a unit and branch on `ENGINE_PLATFORM_LINUX`/`ENGINE_PLATFORM_MACOS` for small differences (executable path, user-data root, debugger detection, `MSG_NOSIGNAL` against `SO_NOSIGPIPE`). A stream may move a difference into `Platform/Linux/` or `Platform/MacOS/` when it grows; the file is named after its unit.
- **Guards:** besides premake's per-OS file selection, every OS file wraps everything after its first two includes in `#if defined(ENGINE_PLATFORM_WINDOWS)` or `#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)` (Architecture Appendix A, CodeStyle §4.1). The OS includes the streams add go inside the guard, so a tool that compiles the file for another host (an IDE indexer, a compilation-database tool) sees an empty file instead of missing OS headers.
- **Pimpl:** the classes that own OS or GLFW resources (`Window`, `Process`, `ProjectLock`, `Socket`, `SocketListener`) keep them behind `struct Impl` and are movable values returned in `Result<T>`, so their public headers carry no OS or GLFW type (§3 rule 3) and the GLFW user pointer stays stable when a `Window` moves.

### 2. Files beyond the Roadmap M2 list

Each one is needed by a frozen header or an acceptance test:

- `Platform/SecureRandom.h|.cpp` and its OS files: ADR 0003 decision 19 has Platform provide the OS entropy for the editor's `UUIDGenerator`; the automation token (§13.2) needs it too.
- `Platform/CrashHandler.cpp`: the host-independent part of decision 1.
- `App/CommandLine.h|.cpp`: `ApplicationSpecification::Args` (§4.1) needs a type, and the Editor and Runtime need one parser that rejects unknown options (exit code 2).
- `Editor/Source/Editor/EditorApp.h|.cpp` and `Runtime/Source/Runtime/RuntimeApp.h|.cpp`: the empty `Application` subclasses the Roadmap asks for, under the names §3 and §14.3 give them; M7 and M10 fill them.
- `Tests/Source/Support/WindowedChild.h|.cpp`: the parent and child halves of `Tests --windowed-child` (decision 13).
- `Tests/Source/Support/Utf8Path.h`: one pair of UTF-8 path conversions for tests, which pass paths to child processes as UTF-8 arguments; it replaces the copies the new test files would otherwise each carry.
- The test files of every unit, `Tests/Source/Editor/EditorAppTests.cpp` and `Tests/Source/Runtime/RuntimeAppTests.cpp`, which run those executables as processes.

### 3. Private members of frozen headers

As in M1 (ADR 0003 decision 22), the contract declares private data members only where a stub uses them, because Clang's `-Wunused-private-field` (an error on the Linux and macOS jobs) fires on unused ones. The streams add the private members and helpers their implementation needs; no public declaration may change without the contract owner's review. The contract did declare the members that make trivial accessors possible: `EngineContext` holds its services by value in construction order, `ProcessContext` its specification, steps and folders, `Application` its specification and context.

### 4. Specification structs give every member an initializer

Clang (`-Wmissing-field-initializers`, part of `-Wextra`) reports a designated initializer that omits a member without a default member initializer, so `{ .Title = "Editor" }` would break the Clang builds. Every member of the M2 specification and data structs therefore has one (`std::string Title{};`), which keeps CodeStyle §3.9's designated initializers usable with only the fields that matter.

### 5. Events and input codes

- **Codes:** `Key`, `MouseButton`, `GamepadButton`, `GamepadAxis` and `KeyModifiers` use GLFW's numeric values, so the window converts with a range check and a cast. The values are never serialized; names are (`"Space"`, `"D0"`, `"Keypad0"`, `"South"`, `"LeftX"`), parsed ASCII case-insensitively like automation enums (§13.4).
- **Event fields:** `KeyEvent::KeyCode` (a member named `Key` of type `Key` is ill-formed for GCC, "changes meaning"); `WindowResizeEvent` carries the window and the framebuffer size together; `GamepadEvent` has a `Kind` (`Connected`, `Disconnected`, `Button`, `Axis`), because GLFW gamepads are polled and automation and replays inject per-control changes (§6.7, §13.6).
- **Axis convention:** events carry engine values: sticks from -1 to 1, up-positive; triggers from 0 (released) to 1 (pressed). The one conversion from GLFW is `GamepadAxisValueFromGlfw` in `InputState.h`, which the window applies to every polled axis; this is how "`InputState` negates `LeftY` and `RightY` when it reads GLFW" (§4.3) is implemented, and it also remaps GLFW's trigger range of -1 to 1 with `(v + 1) / 2`.
- **Why triggers rest at 0:** with GLFW's range a released trigger reads -1, so an Axis action bound to `Gamepad.RightTrigger` would read -1 at rest (its magnitude beats the keys' 0 in the max-magnitude rule of decision 7), and a disconnected gamepad, whose axes are zeroed, would leave the trigger half pressed. With 0 as the rest value of every axis, the dead zone, disconnection and the action rules treat triggers like sticks. Replays and automation store engine values (`GamepadAxis {Value}`, §6.7, §13.6), so the range is part of those formats; the stick table test has trigger rows.
- **Helpers:** `Overloaded`, `IsHandled`, `SetHandled` and `GetEventName` live in the header-only `Events.h`.

### 6. Input latching belongs to the simulation, not to the frame loop

§4.3 says `LatchStep()` runs at the start of every fixed step and `LatchFrame()` once per frame before `OnUpdate`; §5.7 puts both calls inside `PlaySession` (step 1 latches after applying the events stamped for the tick). If `FrameLoop` latched before `OnFixedStep`, a play session's tick-stamped events would arrive one step late and taps would lose an edge. `FrameLoop` therefore only injects polled events into `InputState`; M7's `PlaySession` latches. In M2 nothing reads the views outside tests.

Other `InputState` rules the contract fixes: queries are total (out-of-range codes and gamepad indexes read as up and zero, and `Inject` ignores them); the first `MouseMoveEvent` sets the position without a delta; a disconnected gamepad releases its buttons with edges and zeroes its axes; text, file-drop and window events do not change the state.

### 7. `InputActionMap` semantics

§4.3 gives the dead zone (0.15) and "combined as the max magnitude"; the table test needs exact rules:

- **Dead zone:** values with a magnitude up to 0.15 become 0, larger ones are rescaled linearly so that the output still reaches ±1 (`sign(v) * (|v| - 0.15) / 0.85`), with no jump at the edge.
- **Combination:** the key value (Positive minus Negative) or the gamepad value, whichever has the larger magnitude; the key value on a tie. `Invert` flips the gamepad axis only.
- **Gamepads:** bindings read every connected gamepad (the largest magnitude, the lowest index on a tie), since v1 has one player.
- **Edges:** a Button action's edges are the union of its bindings' edges. Every query is defined for both action types, so a script never meets an assert.
- **Indexing:** actions are indexed in byte-wise name order, whatever the definition order, and validation reports every problem at once as `ErrorIssue`s with pointers `/<Action>/<Field>[/<i>]`.
- **Not in M2:** injected `Action` events (§6.7, §13.6) need a write path that is not a device event; M7's contract adds it with `input.inject`. The same holds for `mouseDelta {Delta}` input (§6.7, §13.6): the `Event` variant has only the absolute `MouseMoveEvent`, the first of which produces no delta, so a relative delta (for example at tick 0 of a replay) cannot be expressed. M7's contract adds a `MouseDeltaEvent { glm::vec2 Delta; bool Handled; }` to `Events.h`, which adds to the views' mouse delta without moving the position, together with the `Action` path. Both are planned changes to the frozen `Events.h` and `InputState.h`, recorded here so that they are not mistaken for drift.

### 8. `GlfwLibrary` may include the Vulkan headers

`glfw3.h` declares `glfwInitVulkanLoader` only after the Vulkan headers, and the Roadmap puts that hook in `GlfwLibrary` (M2). `ModuleRules.json` now allows `Vulkan` among Platform's private third-party headers, restricted to `Platform/GlfwLibrary.cpp` (which includes `vulkan/vulkan_core.h`, not `vulkan.h`, so no `<windows.h>`). The public header spells `PFN_vkGetInstanceProcAddr` as `VulkanGetInstanceProcAddrFunction` without Vulkan types. M2 passes no loader (no renderer yet); M5 passes the engine's.

The swapchain and device selection of M5 need three more GLFW calls: `glfwGetRequiredInstanceExtensions`, `glfwGetPhysicalDevicePresentationSupport` and `glfwCreateWindowSurface` (§8.1: "Our own code: glfwCreateWindowSurface"). They take and return Vulkan types, and nothing in M2 can test them, because there is no Vulkan instance before M5. The contract therefore does not wrap them in Platform. Instead, M5's contract grants GLFW (with the Vulkan headers) to the Graphics device-setup and swapchain files in `ModuleRules.json`, which AGENTS.md already names as the one place outside `Platform/<OS>/` with platform code. Those files reach the window through `Window::GetNativeHandle`, whose comment names them. No public header exposes a GLFW or Vulkan type either way.

### 9. `ProcessContext`

- **Steps in M2:** Log → Profiler → CrashHandler (with the fatal-error handler) → Glfw. Luau flags, `PhysicsEngine` and the Vulkan loader join at the places §4.1 gives, with their milestones. Each step logs `Process context: <Step> initialized` and `Process context: shutting down <Step>` at Info, which is how a child process proves the reverse teardown.
- **Fatal-error handler:** it is the process's only Core `FatalErrorHandler` (a bare function pointer, restored at teardown), so everything §4.6 asks of a fatal error goes through it, in this fixed order: (1) for `FatalErrorKind::Assert` with a debugger attached, break into it (ADR 0003 decision 21); (2) the application's hook; (3) the crash report through `CrashHandler::WriteFatalErrorReport`; (4) in a windowed process, a message box naming the error. Then Core's `FatalError` flushes and exits.
- **Hook:** `ProcessContext::SetFatalErrorHook(FatalErrorHook)` takes a function pointer plus a `void*` user data (Core's handler has no user data, and a second handler would replace the first or need a new global). The editor's autosave (§4.13, M10) and the editor's part of `--gpu-inject-fault` (M5) install it; it must not touch the GPU, wait for other threads or call `FatalError`. Setting and calling share a lock, so a removed hook is never called afterwards. Applications reach the context through the protected `Application::GetProcessContext()`. Running the hook before the report means the report's last log lines include what the hook logged.
- **Message box:** the Platform call that shows it arrives with M5, which first needs it (no Vulkan loader or device, §8.1, §14.3). Step 4 does nothing until then; its place in the order is fixed now.
- **One per process:** a second `Create` is asserted ("a ProcessContext already exists"). `ProcessContext::GetCurrent()` is a global accessor, so its callers are restricted by documentation and review: the App module (`RunApplication`, `Application::Run`), the Tests main and tests. Applications use `Application::GetProcessContext()`, and engine code still receives its services explicitly (§4.1).
- **Specification:** `UserDataRoot` overrides the OS root (tests); `LogToFile` and `WriteCrashReports` follow the rule of decision 10 in Tests children.
- **Construction:** `Create` is the only way in; a public nested `ConstructionKey` with a private constructor restricts the constructor that `CreateScope` must reach (the same for `EngineContext`), because `Scope<T>(new T)` is banned.

### 10. The application name, `ENGINE_PRODUCT_NAME` and log files

- **Define:** `ENGINE_PRODUCT_NAME` did not exist yet. `ApplyFirstPartySettings()` defines it from `WorkspaceName` for executables only (`kind:ConsoleApp or WindowedApp`): the Editor, the Runtime and Tests. The Engine and EditorCore libraries never see it, so engine code cannot put an exported game's files into the engine's folder.
- **The M2 Runtime** names itself `ENGINE_PRODUCT_NAME` until M7's `RuntimeApp` reads the manifest `Name` (§14.3); there is no manifest before M7.
- **Tests child processes** log to the console only. Their parent captures it, and two processes must not write and rotate one `Tests.log` (on Windows a rotation fails while another process holds the file, and the sink then truncates it, ADR 0003 decision 21). The top-level run writes `<UserData>/<ENGINE_PRODUCT_NAME>/Logs/Tests.log` (§4.4), which ADR 0003 decision 25 deferred to M2.
- **Which process is a child** is never guessed from its mode. Many children run no child mode: the M1 ChildTargets children start as `--no-skip --test-case=<name>`, and the process tests start `--list-test-cases` children. Every parent therefore marks its child with `--child-process` (`Test::ChildProcessOption`): `Test::RunChildProcess` appends it, and so do `Test::RunWindowedChild` and `Test::MakeTestsChildSpecification`, which builds the `ProcessSpecification` for tests that start the Tests executable through `Process`. `TestOptions::IsChildProcess()` is true for `--child-process` and for every child mode. The Tests main then sets `LogToFile = !IsChildProcess()` and `WriteCrashReports = !IsChildProcess() || !UserDataDirectory.empty()`: a child writes crash reports only into the user-data root its parent gave it (the crash-child and fatal-error-hook tests), so the expected deaths of death tests and recording-assert-handler targets (which end in `FatalError(Assert)`) never leave a report in the user's real `Crashes/` folder.
- **Editor and Runtime process tests** pass `--user-data-dir=<temporary directory>` (decision 12), so the executables' log files and crash reports land in the test's temporary directory, as CodeStyle §14 requires; the Runtime test checks that its log file is there. A user-data root that is a regular file also gives a cheap test of `RunApplication`'s exit code 3.
- **User-data roots:** `%LOCALAPPDATA%` on Windows (§13.2 already names it), `$XDG_DATA_HOME` or `~/.local/share` on Linux, `~/Library/Application Support` on macOS. `Paths::ValidateAppName` applies the strictest host's rules on every host, because a manifest `Name` is user input.

### 11. `ExitCode` is a struct of `int` constants

`RequestExit(int exitCode = ExitCode::Success)` (§4.1) needs an `int`, an `enum class` cannot be one without a cast, and sub-namespaces are limited to five names. `ExitCode` is a non-instantiable struct of `static constexpr int` members, with `static_assert`s that `InitFailed` and `Crash` equal Core's `FatalInitFailedExitCode` and `FatalCrashExitCode` (ADR 0003 decision 2). It is header-only and implemented by the contract.

### 12. Application, frame loop and command line

- **Factory:** `ApplicationFactory` is `Result<Scope<Application>> (*)(std::span<const std::string> arguments)`. It runs before the `ProcessContext` exists, because its specification decides how the process is set up: the `WindowMode` from the command line and, from M7, the app name of the user-data folder, which the exported runtime reads from `Game.json` (§14.3). It may therefore read files, but must not log except through the fallback logger and must not touch GLFW; the application's constructor only stores its specification.
- **Factory errors** are mapped by code: InvalidArgument (a bad command line) exits with `UsageError` (2), every other code (NotFound, Io, Parse, Validation for a missing or invalid required file) with `InitFailed` (3), as §4.1 lists "required file missing" under 3 and M7 requires `test_runtime_missing_manifest_exits_3`. M7 then only fills in the factory.
- **Last-resort boundary:** `RunApplication`'s allowlisted try/catch covers the factory, `ProcessContext::Create`, `Run` and teardown (steps 2 to 5), not only `Run`. Each of them can allocate and so throw `std::bad_alloc`; an exception escaping `main` would end in `std::terminate`, whose `abort` exits with 3 on Windows and would report a crash as InitFailed. Before the context exists the catch prints through the fallback logger and `FatalError` exits with 4 without a crash report.
- **Engine options:** `--headless` (Headless and `ManualClock`, §13.9), `--frames N` (§13.9) and `--user-data-dir <absolute path>` (`ApplicationSpecification::UserDataRoot`, passed to `ProcessContextSpecification::UserDataRoot`; for process tests) are parsed by `GetEngineCommandLineOptions`/`ApplyEngineCommandLine` for both executables. `--user-data-dir` does not exist in Dist, which honours only the options §13.9 lists, so it is an unknown option there; M7's exported-build tests that need it run Release testing exports.
- **Command-line errors:** `CommandLine` rejects unknown options, missing values, values on flags and repeated options with InvalidArgument, which `RunApplication` turns into exit code 2. There are no single-dash options, so `-headless` is a mistyped option, not a positional argument, and both M2 factories reject positional arguments, since neither declares any. A typo is never a silently ignored flag.
- **Throttle:** `ApplicationSpecification::ThrottleHeadless` (default true) sets `FrameLoopSpecification::ThrottleToFixedHz` for headless runs, as §4.2 requires of headless play without lockstep; the in-process unit tests turn it off, because test runs are unthrottled and must not sleep. The `--headless --frames N` process tests stay throttled (10 frames take about 170 ms).
- **Runtime control of the loop is deferred:** lockstep (`play.step` steps per frame, M7), time scale (M7), a per-suite `ScriptedClock` (M14) and turning the throttle off for lockstep need to change a running `FrameLoop`, which `FrameLoop.h` does not offer yet (it builds its clock and throttle once, and steps with `StepExactly(1)` or `Advance(delta, 1.0)`). The M4 and M7 contracts extend `FrameLoop.h` (for example `SetThrottleToFixedHz`, `SetTimeScale`, `SetClock`, a step request) and add a protected accessor on `Application`; this is a planned change to a frozen header, recorded here.
- **`FrameLoop`** calls the application through `IFrameLoopClient`, which `Application` implements privately, so the §4.2 sequence is unit-tested with a recording client. In M2 a frame is: events (or `WaitEventsTimeout(FixedDelta)` while minimized), `MainThreadQueue::Drain`, steps (`StepExactly(1)` with a `ManualClock`), update, frame count and optional `FixedHz` throttle. Each event goes to the application first; unless it marks the event handled, it reaches `InputState`, and an unhandled `WindowCloseEvent` ends the loop with Success. Automation pumping (M4) and rendering with the frame-boundary catch (M5) join later. `Run` logs `Frame loop started: <ClockKind> clock, <FixedHz> Hz`, which the Editor and Runtime process tests read.
- **Deferred hooks:** `OnRender(RenderContext&)` and `OnImGuiRender()` arrive with M5, because `RenderContext` does not exist yet. `RendererMode` and `GraphicsSpecification` arrive with M5 as well. A Scripted clock is chosen per FeatureTest suite (M14), never through `ApplicationSpecification`.
- **Contexts:** in M2, `EngineContext` holds the VFS (with `user://` when a directory is given, without `.bak` files, ADR 0003 decision 16), `MainThreadQueue`, `JobSystem`, `EventLog`, `InputState` and an optional `Window`. A context without a window needs no GLFW. Later services are appended, never pre-declared.

### 13. Windowed children are ChildTargets test cases

§4.1 runs windowed tests in `Tests --windowed-child=<name>`, "spawned like a death test". The target is an ordinary doctest case in `Test::ChildTargetSuite`, permanently skipped in the normal run, so it uses `CHECK`/`REQUIRE`. In the child, the Tests main creates a windowed `ProcessContext`, runs exactly that case with skips lifted, and exits with doctest's result (2 when the name selects nothing). This adds windowed-child targets to the kinds of permanent skip that ADR 0004 section 5 lists; they are recognized by the same suite decorator.

- **New Tests options:** `--windowed-child=<case>`, `--crash-child` (sets the `FramePhase` breadcrumb to "Crash child", then `CrashHandler::SimulateCrash`, or with `--child-argument=fatal-error` a `FatalError(DeviceLost)`), `--child-process` (decision 10), `--user-data-dir=<absolute path>` (the child's user-data root, so its crash reports land in the parent's temporary directory) and `--child-argument=<text>` (input for a child body, such as the lock file the lock holder takes). At most one child mode per run.
- **Executables of the same build:** `Test::GetBuiltExecutablePath(project)` finds `bin/<OutputDir>/<Project>/<Project>[.exe]`, and `Tests/premake5.lua` makes Tests depend on Editor and Runtime, so building Tests builds them.
- **Displays:** windowed tests are always required. Windows and macOS runners have a display. The Linux CI jobs run the unit stage in an Xvfb display **with a window manager**: on X11, `glfwIconifyWindow` only sends `WM_CHANGE_STATE` to the window manager, and GLFW reports a window as iconified only once the manager sets `WM_STATE` to Iconic, so under bare `xvfb-run` the acceptance test "FrameLoop: a minimized window idles" could never pass (nor M5's minimize test). The jobs install `xvfb openbox x11-utils` and run the unit stage as `xvfb-run -a sh -c 'openbox --sm-disable & until xprop -root _NET_SUPPORTING_WM_CHECK >/dev/null 2>&1; do sleep 0.1; done; exec python3 Scripts/CI.py --stages unit ...'`, so the manager runs before the first test starts. Any lightweight EWMH window manager works. Linux desktops already run one, and developers without a display use the same command, so `Scripts/Setup.py`, which checks what building needs, does not check for `xvfb` or `openbox`.
- **Wall-clock exception:** the windowed child "FrameLoop: a minimized window uses little CPU time per second" measures CPU time per wall-clock second, because that is the Roadmap acceptance criterion; it is the one test that reads the wall clock (CodeStyle §14 forbids it otherwise). Sixty frames of `WaitEventsTimeout(FixedDelta)` take about one second; the test requires more than 0.5 s of wall time and less than 0.25 CPU seconds per second. A spinning loop uses about 1.0, an idling one well under 0.05, so the threshold tolerates a slow or loaded CI runner by a factor of five while still failing a loop that spins. Loosening it needs a new entry here.

### 14. Crash handler details

Reports are text files `crash-<UTC epoch seconds>-<pid>.txt` with fixed sections (Reason, App, Build, Process, Breadcrumbs, Stack trace, Last log lines), plus a minidump on Windows. The last 256 log lines and the breadcrumbs are copied into buffers preallocated at install, the log lines by a `Log` listener, because the ring buffer is locked and cannot be read from a signal handler. The handler prints `Crash: <reason>; report written to <path>` to stderr and exits with 4. `CrashHandler::SimulateCrash()` raises a real access violation or `SIGSEGV`, so the crash child exercises the real path without undefined behaviour in test code. The `VK_EXT_device_fault` text of §4.13 arrives with the device in M5.

### 15. Project lock details

The lock file holds the holder's pid in decimal and one LF. Windows locks one byte at offset 4096 so that the text stays readable; POSIX uses `flock`, which is advisory. A held lock is `AlreadyExists` with the message "'<file>' is locked by process <pid>". The directory must exist: `Library/` is created by the project manager (M4).

### 16. Polling watcher and sockets

- **`PollingFileWatcher`** works on the VFS. `Poll(nowSeconds)` takes the time from the caller, so tests on a `MemoryMount` are exact and nothing reads the wall clock. A change is reported once, as the net change against the last reported state, by the first poll at least `DebounceSeconds` after the scan that last saw the file change. Results are sorted by path, `MarkKnown` implements §7.5's no-echo rule, and the 500 ms schedule on a job is the caller's (M6).
- **Sockets** are loopback-only by construction (no address parameter). Every blocking call takes a timeout, so the automation I/O thread can stop between calls. Winsock is started per object (`WSAStartup` is reference-counted by the OS), which avoids process-level state, and a closed peer is an Io error, never `SIGPIPE`.
- **Socket threads:** §13.2 has one I/O thread serve the listener and up to 4 clients while requests run on the main thread, which answers. A rule of one thread per object would force that thread to poll five objects in turn, each with its own timeout, and leave no way to send a response while it sits in `Receive`. So a connection is full duplex: one `Send` and one receiving call (`Receive` or `WaitAny`) may run at the same time on two threads (the OS supports this on one socket), and `IsOpen` alongside them; `Close`, moves and destruction need exclusive access. `Socket::WaitAny(sockets, listener, timeout)` waits on the listener and every connection at once (`poll`, `WSAPoll`) and reports which are ready (`SocketReadiness`). The I/O thread waits with a short timeout, checks its stop flag, accepts and receives; the main thread sends responses with `Send` directly, so the I/O thread never needs waking for outgoing data, and the watchdog's `Busy` answers come from the I/O thread itself, so the M4 server serializes the Sends of one connection with its own lock. Closing a socket from another thread to unblock a reader is not supported: on POSIX the descriptor could be reused while the reader still uses it.

### 17. Build files

- Every executable links the Windows libraries behind `Platform/Windows`: `dbghelp`, `ws2_32`, `bcrypt`, `shell32` and `ole32` (`ApplyFirstPartySettings()`). Linux and macOS need nothing beyond libc and libSystem.
- The contract task made every build-file change M2 is expected to need: `ENGINE_PRODUCT_NAME`, the system libraries, Tests' dependency on Editor and Runtime, and the Vulkan rule of decision 8. The streams should need none, apart from the `TestsChildProcess` removal (decision 18).

### 18. Stream ownership

Each workstream owns the files of its units, and private files it adds are named after those units:

- **A (window and input):** `Platform/Events.h`, `Platform/Input/{KeyCodes,InputState,InputActionMap}.h|.cpp`, `Platform/Window.h|.cpp`, `Platform/GlfwLibrary.h|.cpp`, and their tests under `Tests/Source/Engine/Platform/` (`EventsTests`, `Input/*Tests`, `WindowTests`, `GlfwLibraryTests`).
- **B (process, crash handler, paths, project lock):** `Platform/{Process,CrashHandler,Paths,ProjectLock,SecureRandom}.h|.cpp`, their `Windows/` and `Posix/` files, their tests, and the migration of `Tests/Source/Support/ChildProcess.cpp` onto `Process` (ADR 0003 decision 1).
- **C (watcher and socket):** `Platform/PollingFileWatcher.h|.cpp`, `Platform/Socket.h`, `Platform/{Windows,Posix}/Socket*.cpp`, and their tests.
- **D (application and frame loop):** `App/*`, `Editor/Source/Editor/{EditorApp.h,EditorApp.cpp,EditorMain.cpp}`, `Runtime/Source/Runtime/{RuntimeApp.h,RuntimeApp.cpp,RuntimeMain.cpp}`, `Tests/Source/TestMain.cpp`, `Tests/Source/Support/{TestOptions,WindowedChild}.h|.cpp` and their tests, `Tests/Source/Support/Utf8Path.h` and its test, and the App, Editor and Runtime tests.

Shared files have one owner each in M2:

- B owns `Scripts/ModuleRules.json`: it removes the `TestsChildProcess` project rule and its `PrecompiledHeaders` entry when `ChildProcess.cpp` moves onto `Process`.
- D owns the premake files (`premake5.lua`, `Dependencies.lua`, `*/premake5.lua`) and `.github/workflows/ci.yml`, where the Linux jobs install `xvfb openbox x11-utils` and run the unit stage in an Xvfb display with openbox running, as decision 13 spells out.
- B keeps `Test::RunChildProcess` appending `--child-process` when it moves `ChildProcess.cpp` onto `Process`; the M1 implementation already does.
- `Scripts/*.py` are unchanged by M2; any change goes through the integration task.

### 19. Child environment variables

`ProcessSpecification::Environment` sets variables for the child on top of the parent's environment, in order (a later entry for the same name wins); a name that is empty or contains `=` or NUL, and a value with NUL, is InvalidArgument. M2 has no use for it beyond its own test, but M5's process tests run the real Editor with `ENGINE_VULKAN_LOADER=missing` and `ENGINE_GPU` (§8.1, Roadmap M5), and M2 establishes that such tests start the executables through `Process::Run`. Adding the field now keeps `Process.h` unchanged in M5.

### 20. Review of the contract

Two reviews of the uncommitted contract produced 25 findings. Accepted and fixed: the child-process marker (decision 10), the window manager for Linux CI and the wall-clock exception (decision 13), the factory's file access and error mapping and the wider last-resort boundary (decision 12), the fatal-error hook and handler order (decision 9), the trigger range (decision 5), socket threading and `WaitAny` (decision 16), `--user-data-dir` for the executables (decisions 10 and 12), `ThrottleHeadless` (decision 12), child environment variables (decision 19), single-dash and positional arguments (decision 12), the injected-event rules of `Window::InjectEvent`, the GetCurrent restriction (decision 9), the `ENGINE_PLATFORM_*` guards (decision 1), and the style findings (line lengths, test names, `{}` initializers in `TestOptions`, `CommandLineOption` lifetimes, one shared `Support/Utf8Path.h` instead of per-file UTF-8 path helpers). Recorded as planned changes instead of contract changes: `MouseDeltaEvent` (decision 7), the Vulkan surface calls (decision 8) and runtime control of `FrameLoop` (decision 12). Rejected: adding `xvfb` and `openbox` to `Scripts/Setup.py`'s Linux check, because building needs neither and a Linux desktop already has a display and a window manager (decision 13).

## Requested amendments (docs owner)

- **Architecture §2.2:** `ENGINE_PRODUCT_NAME` is defined for executables only, and the Windows executables link the system libraries of decision 17 (decision 10).
- **Architecture §3:** Platform's per-OS file layout, guards and pimpl rule (decision 1); `GlfwLibrary.cpp` may include `vulkan/vulkan_core.h`, and M5 grants GLFW to the Graphics device-setup and swapchain files (decision 8).
- **Architecture §4.1:**
  - `ApplicationSpecification` without `RendererMode`/`GraphicsSpecification` until M5, plus `UserDataRoot`, `WindowSettings`, `MaxFrames`, `WorkerCount` and `ThrottleHeadless`; the factory signature, its file access and error mapping, the last-resort boundary around steps 2 to 5, and the engine options including `--user-data-dir` (decision 12).
  - The M2 `ProcessContext` steps, their log lines, the fatal-error handler order and hook, `GetCurrent` and its callers, `Application::GetProcessContext` and the specification flags (decision 9).
  - `ExitCode` as a struct of `int` constants (decision 11).
  - The `EngineContext` services of M2 (decision 12).
- **Architecture §4.2:** `FrameLoop` and `IFrameLoopClient`, the event dispatch order, that the frame loop injects but does not latch, and the headless throttle switch (decisions 6 and 12).
- **Architecture §4.3:** the event field names, `GamepadEvent::Kind`, `GamepadAxisValueFromGlfw`, the trigger range 0 to 1, the `InputState` rules, the `InputActionMap` dead-zone, tie, gamepad and edge rules, and that automation input bypasses the window (decisions 5 to 7).
- **Architecture §4.6:** the fatal-error handler order (debugger, application hook, crash report, message box) (decision 9).
- **Architecture §4.4:** the user-data roots per OS, `Paths::ValidateAppName`, and console-only logging in Tests child processes (decision 10).
- **Architecture §4.13:** the crash report format, `SimulateCrash`, and the project-lock file format and error (decisions 14 and 15).
- **Architecture §7.5:** the watcher takes the time from its caller (decision 16).
- **Architecture §13.2:** socket threading (full duplex, `Socket::WaitAny`) (decision 16).
- **Architecture §13.9:** `--user-data-dir` for the Editor and Runtime outside Dist (decision 12).
- **Architecture §15.2:** windowed children are ChildTargets test cases; the Tests options `--windowed-child`, `--crash-child`, `--child-process`, `--user-data-dir` and `--child-argument`, and which child writes which file (decisions 10 and 13); Tests depends on Editor and Runtime (decision 13).
- **Architecture §15.8:** the Linux CI jobs run the unit stage in an Xvfb display with a window manager (decision 13).
- **CodeStyle §14:** the minimized-window test is the one wall-clock exception (decision 13).
- **Roadmap M2 deliverables:** the files of decision 2.
- **ADR 0004 section 5:** windowed-child targets are a kind of child-process target (decision 13).
