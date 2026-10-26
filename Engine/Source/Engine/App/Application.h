#pragma once

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/FrameLoop.h"
#include "Engine/Audio/AudioEngine.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Core/Time.h"
#include "Engine/Graphics/GraphicsSpecification.h"
#include "Engine/Platform/Events.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Window.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

// The application lifecycle (Architecture §4.1). There is no layer stack: EditorApp and RuntimeApp are the only
// subclasses, and engine code never reaches the application through a global (no Application::Get()).

namespace Engine {

#if !defined(ENGINE_DIST)
	class ImGuiLayer;
#endif
	class ProcessContext;
	struct GpuDeviceLossReport;
	struct RenderContext;

	struct ApplicationSpecification
	{
		// The application name: the user-data folder (§4.4) and the default window title.
		std::string Name{};
		// --user-data-dir: the user-data root that replaces the OS's (ProcessContextSpecification::UserDataRoot), so process
		// tests keep the executables' logs and crash reports in a temporary directory. Absolute; empty: the OS's.
		std::filesystem::path UserDataRoot{};
		// The parsed command line.
		CommandLine Args{};
		// Windowed (native GLFW platform) or Headless (GLFW null platform), chosen once per process (§4.1): RunApplication
		// creates the ProcessContext with it, and Run asserts that it equals the ProcessContext's mode.
		WindowMode Window = WindowMode::Windowed;
		// The main window, created in both modes (a null-platform window when headless); an empty Title means Name.
		WindowSpecification WindowSettings{};
		// FixedHz 60, MaxStepsPerFrame 5, MaxFrameDelta 0.25.
		FrameLoopConfig Loop{};
		// System for windowed runs, Manual for headless runs and lockstep (§4.2, §13.9). Scripted needs a delta table and
		// is chosen by the FeatureTest runner per suite, never here: Run logs an error naming the Scripted clock and
		// returns InitFailed before OnInitialize.
		ClockKind Clock = ClockKind::System;
		// --frames N: Run returns ExitCode::Success after N frames.
		std::optional<uint64_t> MaxFrames{};
		// Headless runs pace their frames to FixedHz per wall-clock second (§4.2: headless play without lockstep never runs
		// unthrottled). The in-process unit tests turn it off, because test runs are unthrottled; windowed runs are never
		// throttled by the loop.
		bool ThrottleHeadless = true;
		// JobSystem workers; nullopt: JobSystem::GetDefaultWorkerCount().
		std::optional<uint32_t> WorkerCount{};
		// The application's own reflected types, registered into the context's TypeRegistry before it is frozen
		// (EngineContextSpecification::RegisterTypes): the editor's automation structs. Null adds nothing.
		RegisterTypesFunction RegisterTypes = nullptr;
		// engine:// and enginecache:// for the context (EngineContextSpecification::EngineResourcesDirectory and
		// EngineCacheDirectory, ADR 0010): development editors pass <repo>/Resources and <repo>/bin/EngineCache. Empty: not
		// mounted.
		std::filesystem::path EngineResourcesDirectory{};
		std::filesystem::path EngineCacheDirectory{};
		// Vulkan (a GraphicsDevice on the process's Vulkan loader, which RunApplication then requires) or None (logic only:
		// no loader, no device, screenshot capabilities Unsupported, §13.9). --renderer.
		RendererMode Renderer = RendererMode::Vulkan;
		// The device's settings (§4.1: validation, GPU override, frames in flight, API cap): --gpu-validation, --gpu,
		// --vulkan-api and --gpu-inject-fault, and (M18) --gpu-reset-wait. Ignored with RendererMode::None.
		GraphicsSpecification Graphics{};
		// --expect-no-gpu-errors: a run whose device reported any error or warning in its whole life (GpuDiagnostics:
		// validation and synchronization-validation messages, NVRHI messages, and the leak report and validation-layer
		// reports of the device's own teardown, read through EngineContext::DestroyGraphics) returns ExitCode::Failed
		// instead of Success, logging both counts at Error level; an exit code other than Success is kept. The loader's
		// notes about the machine's installation are not counted (IsLoaderInstallationMessage). The GPU tests pass it to
		// every Editor and Runtime process they start, so a validation message inside such a process fails the test, as it
		// fails an in-process GPU test (§15.3). Ignored with RendererMode::None.
		bool ExpectNoGpuErrors = false;
#if !defined(ENGINE_DIST)
		// Dear ImGui for the application's own UI (OnImGuiRender, §8.11): the editor turns it on, and a windowed non-Dist
		// Runtime for its F1 stats overlay. Needs RendererMode::Vulkan; ignored otherwise. Dist builds have no ImGui at all
		// (§14.4; Docs/Decisions/0024-m15-contract.md decision 11: the Dist map check names any ImGui symbol), so this
		// member, ImGuiIniPath, OnImGuiRender, GetImGuiLayer and the layer itself are compiled out there.
		bool EnableImGui = false;
		// imgui.ini while no project is open, relative to the user-data folder (user://, §8.11): the editor's
		// "Editor/imgui.ini". Run resolves it to the native path under ProcessContext's user-data root. Empty: no ini file.
		std::string ImGuiIniPath{};
#endif
		// M7: the exported game's Engine.pak (EngineContextSpecification::EnginePak): mounted as engine:// instead of
		// EngineResourcesDirectory, and its Shaders/ are the ShaderLibrary's root in every configuration, so an exported game
		// renders with the target configuration's SPIR-V (§2.2, §14.1). The Runtime sets it from its manifest; empty: none.
		std::filesystem::path EnginePak{};
		// M7, --expect-no-errors (§13.9, every configuration, Dist included): a run during which any Error or Critical entry
		// was logged (every logger, the Script logger included) returns ExitCode::Failed instead of Success, logging the count
		// at Error level; an exit code other than Success is kept. The export smoke test and the exported-runtime tests pass
		// it (§14.2 step 7).
		bool ExpectNoErrors = false;
		// M12: the context's AudioEngine (EngineContextSpecification::Audio, §10.1); nullopt: GetDefaultAudioSpecification of
		// Window. Every application has an engine; tests that start windowed applications in-process ask for
		// AudioDeviceKind::None, and windowed Editor and Runtime processes that tests start pass --audio-device none.
		std::optional<AudioEngineSpecification> Audio{};
		// M15, --log-level <trace|info|warn|error|critical> (§13.9: Dist honours it; Docs/Decisions/0024-m15-contract.md
		// decision 17, stream D): the minimum level of the output sinks, the console and the log file, for the whole run, like
		// LogSpecification::ConsoleLevel; nullopt: the configuration's default (§4.4). It filters output only: the loggers
		// keep their level, so --expect-no-errors (which counts every Error and Critical entry), the ring buffer that log.read
		// serves, log listeners and expected-log capture see every entry whatever this says. In Dist, where Trace logging is
		// compiled out, "trace" behaves as "info" (ApplyEngineCommandLine stores Info). RunApplication hands it to the
		// ProcessContext (ProcessContextSpecification::LogOutputLevel), so the process's own lines are filtered from the
		// first; Run also sets it (Log::SetOutputLevel) from its start to its end and then restores the previous level, so an
		// application run in place, as the Tests do, is filtered alike.
		std::optional<LogLevel> MinimumLogLevel{};
	};

	// The audio engine an application gets by default (§10.1): windowed runs play through the system's default device with
	// miniaudio's job thread (AudioDeviceKind::System, AudioDecoding::Threaded); headless runs have no device and decode
	// deterministically (AudioDeviceKind::None, AudioDecoding::Deterministic), pulling frames per frame or per tick.
	[[nodiscard]] AudioEngineSpecification GetDefaultAudioSpecification(WindowMode window);

	// The options every application accepts, handled by ApplyEngineCommandLine:
	//   --headless              Window = Headless, Clock = Manual (all headless runs use ManualClock, §13.9)
	//   --frames N              MaxFrames = N, N >= 1 (stub runs and smoke tests, §13.9, §14.2)
	//   --expect-no-errors      ExpectNoErrors = true (M7)
	//   --user-data-dir <path>  UserDataRoot = path, absolute (process tests)
	//   --renderer <mode>       Renderer = Vulkan for "vulkan", None for "none" (§12.1, §13.9)
	//   --gpu-validation[=sync] Graphics.Validation = true (§2.2: on by default in Debug, never in Dist); with "=sync" also
	//                           Graphics.SynchronizationValidation = true (§15.3)
	//   --expect-no-gpu-errors  ExpectNoGpuErrors = true
	//   --vulkan-api <version>  Graphics.MaxApiVersion: "1.3" or "1.4" (§8.1)
	//   --gpu <index|name>      Graphics.GpuOverride (§8.1; ENGINE_GPU when absent)
	//   --gpu-inject-fault <f>  Graphics.InjectFault: "device-lost", "oom-texture", "hang", "oom-specialization",
	//                           "device-lost-unrecoverable" (§8.14 item 8; M18, Docs/Decisions/0029-m18-contract.md
	//                           decision 30), "hang-reset", "hang-reset-persistent", "oom-rendertarget",
	//                           "oom-rendertarget-persistent", "oom-pipeline", "oom-pipeline-persistent", "oom-swapchain" or
	//                           "oom-swapchain-persistent" (M18, decision 46)
	//   --gpu-reset-wait <ms>   Graphics.ResetWaitMilliseconds: 0 to MaxGpuResetWaitMilliseconds (120000), the GPU reset
	//                           window a hang's reset wait and a device-loss recovery's retry wait (M18, decision 41; also in
	//                           Dist)
	//   --audio-device <kind>   Audio = GetDefaultAudioSpecification(Window) with Device System for "system", Null for
	//                           "null" or None for "none" (M12: windowed processes that tests start never open the
	//                           machine's audio device, §15.1 T1); the decoding mode stays the window mode's
	//   --log-level <level>     MinimumLogLevel: "trace", "info", "warn", "error" or "critical", the output sinks' minimum
	//                           (M15, stream D; also in Dist, where "trace" stores Info)
	// Every option except --headless, --frames, --expect-no-errors, --log-level and --gpu-reset-wait is absent from Dist
	// builds, where it is an unknown option (§13.9 lists the options Dist honours).
	// An application parses its command line with these plus its own options (CommandLine::Parse). The returned options
	// refer to static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetEngineCommandLineOptions();

	// Applies the engine options of `commandLine` to `specification` (see GetEngineCommandLineOptions) and stores the
	// command line in specification.Args. Errors: InvalidArgument naming the option for a --frames value that is not an
	// integer >= 1, a --user-data-dir value that is not an absolute path in valid UTF-8, a --gpu-reset-wait value that is
	// not an integer from 0 to MaxGpuResetWaitMilliseconds, or a --renderer, --gpu-validation, --vulkan-api,
	// --gpu-inject-fault, --audio-device or --log-level value other than the spellings listed above (the message lists them).
	[[nodiscard]] Status ApplyEngineCommandLine(const CommandLine& commandLine, ApplicationSpecification& specification);

	// The base of the editor and runtime applications. Not copyable or movable.
	//
	// Run, on the main thread, with the ProcessContext alive (asserted):
	//   1. Per-context initialization (§4.1 level 2): EngineContext::Create with the main window, the worker count, the
	//      RegisterTypes function, the ProcessContext's user-data folder as user://, with RendererMode::Vulkan the
	//      GraphicsSpecification, and the audio specification (Audio, or GetDefaultAudioSpecification; M12); then the frame's
	//      rendering objects (a Swapchain on the window in a windowed process, an OffscreenTarget of the window's
	//      framebuffer size in a headless one, the FramePacer, the GpuProfiler, and, except in Dist, the
	//      ImGuiLayer when EnableImGui is set, with ImGuiIniPath under the user-data folder); a clock from the
	//      specification (SystemClock, or ManualClock with the loop's FixedDelta); then OnInitialize. A failure is logged
	//      at Error level (and shown in an error dialog when the ProcessContext has ShowErrorDialogs), whatever was built
	//      is torn down in reverse, and Run returns ExitCode::InitFailed, with one exception (§8.14 item 7): a Gpu error
	//      creating a render target, a framebuffer or a pipeline of the rendering objects (the device is out of memory at
	//      startup) ends the process through FatalError(OutOfMemory), exit code 4. Unsupported (no surface format the
	//      swapchain accepts) stays InitFailed.
	//   2. The frame loop (FrameLoop; FrameLoopSpecification::ThrottleToFixedHz for headless runs with ThrottleHeadless),
	//      until RequestExit, an unhandled window close (ExitCode::Success) or MaxFrames (ExitCode::Success). The hooks
	//      below run inside it. After OnUpdate, each frame updates the context's AudioEngine (AudioEngine::Update with the
	//      frame clock's accumulated unscaled time and the frame's unscaled delta, M12). With a device, each frame renders
	//      (§8.2): FramePacer::BeginFrame and GpuProfiler::BeginFrame, the swapchain image or the offscreen target, a command
	//      list cleared to FrameClearColor (Graphics/RenderContext.h), OnRender, then with ImGui ImGuiLayer::BeginFrame (with
	//      the time since the last UI frame), OnImGuiRender, EndFrame and Render into the same target; the submission (the
	//      swapchain's semaphores queued before it), Present, FramePacer::EndFrame and GraphicsDevice::RunGarbageCollection.
	//      A frame the swapchain skips (minimized or just recreated) records and submits nothing, calls neither OnRender nor
	//      OnImGuiRender, and passes GraphicsDevice::GetLastSubmissionID to FramePacer::EndFrame. A headless frame target
	//      replaced by a resize stays alive until every frame that cleared it has completed. A Gpu error from
	//      ImGuiLayer::Render (a pipeline, buffer or binding set could not be created) ends the process through
	//      FatalError(OutOfMemory) (§8.14 item 7); other Render errors are logged once per run of failing frames.
	//   2b. M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 28, 29 and 33): device loss. A frame on a lost
	//      device (GraphicsDevice::IsLost) renders nothing: no BeginFrame wait, no OnRender, no OnImGuiRender, no
	//      submission or Present, as for a frame the swapchain skips (the simulation, its scripts and its audio still
	//      run). A Gpu error on a lost device is the loss, never FatalError(OutOfMemory). At the frame boundary
	//      (FrameLoop's OnFrameDeviceLost) the application recovers the device (a frame the loss ended while it
	//      recorded first has its command list closed and executed on the lost device, so NVRHI retires it):
	//      OnDeviceLost with the loss's report (the editor writes its autosave there), then
	//      EngineContext::RecoverGraphics, which releases every registered owner (the rendering objects of step 1
	//      register as one owner, "Application frame", and the ImGuiLayer's renderer as another), creates the device
	//      again and creates them again. On success it appends one EngineEventType::GpuDeviceLost event (Message: the
	//      report's context and fault description) to the context's EventLog, calls OnDeviceRecovered, and the next
	//      frame renders; on failure it ends the process through GraphicsDevice::RaiseDeviceLost with the report and
	//      the recovery's error (FatalError(DeviceLost): the fatal hook's autosave, a crash report, exit code 4). A
	//      loss during initialization, before the first frame, ends the process the same way (decision 25). The
	//      recovered frame's objects are new (a FramePacer counts its frames from 0), but RenderContext::FrameIndex
	//      keeps growing: the first frame after a recovery takes the first multiple of the frames in flight after the
	//      last frame's index, so FrameSlot stays FrameIndex modulo the frames in flight.
	//   2c. M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40, 42 and 44): hangs and out-of-memory. A hang that
	//      the driver or the operating system resets during a bounded wait's reset wait (FramePacer.h) is a loss that step
	//      2b recovers. The frame's resize after startup (the swapchain's recreation, the headless offscreen target's and the
	//      forced UI target's Resize) is a request: an out-of-memory creation is trimmed and retried once (RetryAfterTrim),
	//      and a second one skips the frame, logs one Warn line, and skips every later frame at that framebuffer size,
	//      windowed and headless alike (headless must equal windowed), until the size changes (a minimized window's
	//      included, so restoring it retries) and the resize is tried again; the run goes on. A swapchain recreation that
	//      fails otherwise stays FatalError(Gpu) (Swapchain.h). ImGuiLayer::Render's out-of-memory in the frame (a geometry
	//      buffer, a binding set) stays FatalError(OutOfMemory), as do the rendering objects of step 1 and their recreation
	//      in a recovery.
	//   3. OnShutdown, GraphicsDevice::WaitForIdle, the rendering objects (ImGuiLayer, GpuProfiler, swapchain or offscreen
	//      target, pacer), then the GPU services (EngineContext::DestroyGraphics) with the ExpectNoGpuErrors check (see the
	//      field), then the rest of the context (reverse order), then the ExpectNoErrors check, which so counts the errors
	//      logged during the whole teardown too. Run returns the exit code.
	// Throughout, MinimumLogLevel (when set) is the output sinks' minimum level (Log::SetOutputLevel), and the previous level
	// is restored when Run returns.
	// Run may be called once per Application.
	class Application : private IFrameLoopClient
	{
	public:
		explicit Application(ApplicationSpecification specification);
		~Application() override;

		Application(const Application&) = delete;
		Application& operator=(const Application&) = delete;

		// Runs the application (see the class comment) and returns the process exit code (§4.1 table).
		[[nodiscard]] int Run();

		// Ends the frame loop after the current frame with `exitCode`; the first request wins. During OnInitialize it
		// makes Run skip the loop (OnShutdown still runs). Main thread.
		void RequestExit(int exitCode = ExitCode::Success);

		// The context; only between the start of initialization and the end of OnShutdown (asserted).
		[[nodiscard]] EngineContext& GetContext();

		[[nodiscard]] const ApplicationSpecification& GetSpecification() const { return m_Specification; }
	protected:
		// Makes the run end with `exitCode`, a failure (not ExitCode::Success, asserted): from the frame loop it ends the loop
		// after the current frame as RequestExit does, and unlike RequestExit it also replaces a Success that an earlier request
		// or MaxFrames chose, so a failure found after the loop decided to end still fails the run (M18's milestone review: the
		// editor's final screenshots taken in OnDeviceRecovered after a device loss on the run's last frame; also usable from
		// OnShutdown). A failure requested earlier, by either call, keeps its code. Main thread, while Run runs.
		void FailRun(int exitCode = ExitCode::Failed);
		// The process's context, for what an application adds at process level: its fatal-error hook
		// (ProcessContext::SetFatalErrorHook, the editor's autosave) and the user-data folders. Only while Run runs, from the
		// start of initialization to the end of OnShutdown (asserted).
		[[nodiscard]] ProcessContext& GetProcessContext();
		// After the context exists and before the first frame. An error makes Run return ExitCode::InitFailed.
		[[nodiscard]] virtual Status OnInitialize() { return {}; }
		// M18 (C18.3, decision 29): at the frame boundary after a device loss, before the recovery releases anything: CPU work
		// only, never the GPU (the editor publishes and writes its autosave, like its fatal hook). Does nothing by default.
		virtual void OnDeviceLost(const GpuDeviceLossReport& /*report*/) {}
		// M18 (C18.3, decision 29): after a successful recovery, before the next frame: what depends on the recreated GPU
		// objects beyond its own registration (the editor asks for a fresh UI frame). Does nothing by default.
		virtual void OnDeviceRecovered(const GpuDeviceLossReport& /*report*/) {}
		// After the last frame, while the context still exists. Runs when OnInitialize succeeded.
		virtual void OnShutdown() {}
		// Every window or injected event, before InputState sees it; mark it handled to keep it from the game.
		virtual void OnEvent(Event& /*event*/) {}
		// Zero or more times per frame, one per fixed step (§4.2).
		virtual void OnFixedStep(const SimStep& /*step*/) {}
		// Once per frame after the steps; frame.Alpha is this frame's interpolation factor (§5.2).
		virtual void OnUpdate(const FrameTime& /*frame*/) {}
		// Once per frame before the steps, after queued main-thread work: the safe point where automation requests run
		// (§4.2 step 3).
		virtual void OnSafePoint() {}
		// Once per rendered frame with a device, after OnUpdate: record the frame's scene work into context.CommandList
		// (§4.1, §8.2). Not called with RendererMode::None or for a frame the swapchain skips.
		virtual void OnRender(RenderContext& /*context*/) {}
#if !defined(ENGINE_DIST)
		// Once per rendered frame with ImGui (EnableImGui), after OnRender, between ImGui::NewFrame and ImGui::Render:
		// submit the frame's widgets. Not called for a frame the swapchain skips; the next UI frame's io.DeltaTime then
		// covers the skipped frames' time too. Not in Dist builds, which have no ImGui.
		virtual void OnImGuiRender() {}
#endif
		// M9: once AFTER executing this frame's scene/UI list and BEFORE Present, with that list's actual submission ID.
		// Main thread; never called for an unrendered/skipped frame. Notify each renderer used by this frame through
		// SceneRenderer::OnSubmitted, then drain queued UI picks against that submitted image/table/generation before a
		// subsequent Render or Resize can replace them. Pick requests from OnImGuiRender only enqueue owned click data.
		// The hook may submit nonblocking copies; frame pacing covers the device's last submission after this hook.
		// Screenshot/thumbnail lists submit and notify their own renderers directly, never call this application hook.
		virtual void OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId);
		// M10: request a complete offscreen scene/UI frame at the next render phase, even while minimized. No swapchain
		// acquire/present in that case; retain the last nonzero UI size. Normal rendering consumes the request too.
		// Main thread, while Run is active. Unsupported without a device/ImGui (always in Dist); does not advance simulation
		// or render here. Forced frames invoke the same render/submission hooks, with no reuse of stale ImGui draw data.
		[[nodiscard]] Status RequestOffscreenUiFrame();

#if !defined(ENGINE_DIST)
		// The application's ImGuiLayer (editor screenshots re-render its last frame, §8.13); nullptr without ImGui, and
		// outside the span from rendering initialization to the end of OnShutdown. Not in Dist builds.
		[[nodiscard]] ImGuiLayer* GetImGuiLayer() { return m_ImGuiLayer.get(); }
#endif

		// M7 play-session controls of the frame loop (FrameLoop::SetTimeScale, SetThrottleSuspended and SetLoopConfig), for the
		// play session's time scale, the unthrottled frames of a running play.step and the project's Simulation.FixedHz and
		// MaxStepsPerFrame while the editor plays (§4.2, §13.6). Only while the frame loop runs (from the first frame's hooks
		// to the last's; asserted): the editor and the Runtime call them at their safe point.
		void SetFrameTimeScale(double timeScale);
		void SetFrameThrottleSuspended(bool suspended);
		void SetFrameLoopConfig(const FrameLoopConfig& config);
		// Main-thread copy of completed loop observations; zeros outside the active loop. Reading never advances time.
		[[nodiscard]] FrameLoopStatistics GetFrameStatistics() const;
	private:
		void OnFrameEvent(Event& event) override;
		void OnFrameSafePoint() override;
		void OnFrameFixedStep(const SimStep& step) override;
		void OnFrameUpdate(const FrameTime& frame) override;
		void OnFrameRender(const FrameTime& frame) override;
		// M18 (C18.3, decision 29): step 2b's recovery.
		void OnFrameDeviceLost() override;

		// Step 1's rendering objects (see the class comment); does nothing with RendererMode::None.
		[[nodiscard]] Status InitializeRendering();
		// M18 (C18.3, decision 33): the "Application frame" owner's two halves. CreateFrameObjects creates the swapchain or the
		// headless offscreen target, the command list, the FramePacer and the GpuProfiler (InitializeRendering, and the
		// owner's Recreate on the recreated device), returning the first error with its context; ReleaseFrameObjects (the
		// owner's Release) ends a UI frame the loss interrupted (ImGuiLayer::EndInterruptedFrame) and drops them all, the
		// forced UI target and the slot targets included, without a GPU call.
		[[nodiscard]] Status CreateFrameObjects();
		void ReleaseFrameObjects();
		// Step 3's teardown of what InitializeRendering built, after GraphicsDevice::WaitForIdle.
		void ShutdownRendering();
		// Logs an initialization failure at Error level, and shows it in an error dialog when `process` has
		// ShowErrorDialogs and the application is windowed.
		void ReportInitializationFailure(const ProcessContext& process, const Error& error) const;

		// The frame clock of the specification (ClockKind::Scripted is rejected before this is called).
		[[nodiscard]] Scope<Clock> CreateClock() const;
		// Step 3's --expect-no-errors check (ApplicationSpecification::ExpectNoErrors): `exitCode`, or ExitCode::Failed when
		// it is Success and an error was logged since Run started (M7).
		[[nodiscard]] int ApplyExpectNoErrors(int exitCode) const;
	private:
		// The frame's rendering objects with a device (swapchain or offscreen target, pacer, profiler, command list),
		// defined in Application.cpp.
		struct FrameRendering;
		// The log listener that counts the Error and Critical entries of a run with ExpectNoErrors, defined in
		// Application.cpp.
		struct LoggedErrorCounter;
	private:
		ApplicationSpecification m_Specification;
		Scope<EngineContext> m_Context;    // between the start of initialization and the end of OnShutdown
		Scope<FrameLoop> m_FrameLoop;      // while the frame loop runs
		Scope<FrameRendering> m_Rendering; // with a device, from InitializeRendering to ShutdownRendering
#if !defined(ENGINE_DIST)
		Scope<ImGuiLayer> m_ImGuiLayer; // with EnableImGui, from InitializeRendering to ShutdownRendering
#endif
		Scope<LoggedErrorCounter> m_LoggedErrors; // with ExpectNoErrors, from the start of Run to its end
		std::optional<int> m_PendingExitCode;     // an exit requested before the frame loop exists (during OnInitialize)
		std::optional<int> m_FailedExitCode;      // FailRun's first failure, which replaces a Success that ended the run
		double m_AudioClockSeconds = 0.0;         // the frame clock's accumulated unscaled time for AudioEngine::Update (M12)
		bool m_HasRun = false;
	};

	// Builds an application from argv[1..] (UTF-8): parses the command line with GetEngineCommandLineOptions plus the
	// application's own options, fills an ApplicationSpecification and constructs the subclass. It runs before the
	// ProcessContext exists, because the specification decides how the process is set up (WindowMode, the app name of the
	// user-data folder, UserDataRoot). So it must not log except through the fallback logger (which prints warnings and
	// errors to stderr) and must not touch GLFW; it may read files, as the exported runtime reads its Game.json for the app
	// name (§14.3). The constructor only stores its specification. Errors, which RunApplication maps by code:
	// InvalidArgument for a bad command line (ExitCode::UsageError, 2); any other code, such as NotFound, Io, Parse or
	// Validation for a missing or invalid required file (ExitCode::InitFailed, 3).
	using ApplicationFactory = Result<Scope<Application>> (*)(std::span<const std::string> arguments);

}
