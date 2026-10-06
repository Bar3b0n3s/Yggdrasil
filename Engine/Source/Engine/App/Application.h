#pragma once

#include "Engine/App/CommandLine.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/FrameLoop.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Clock.h"
#include "Engine/Core/FixedStepScheduler.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"
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

	class EngineContext;
	class ProcessContext;

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
	};

	// The options every application accepts, handled by ApplyEngineCommandLine:
	//   --headless              Window = Headless, Clock = Manual (all headless runs use ManualClock, §13.9)
	//   --frames N              MaxFrames = N, N >= 1 (stub runs and smoke tests, §13.9, §14.2)
	//   --user-data-dir <path>  UserDataRoot = path, absolute (process tests); not in Dist, where it is an unknown option
	//                           (§13.9 lists the options Dist honours)
	// An application parses its command line with these plus its own options (CommandLine::Parse). The returned options
	// refer to static storage.
	[[nodiscard]] std::span<const CommandLineOption> GetEngineCommandLineOptions();

	// Applies the engine options of `commandLine` to `specification` (see GetEngineCommandLineOptions) and stores the
	// command line in specification.Args. Errors: InvalidArgument naming the option for a --frames value that is not an
	// integer >= 1 or a --user-data-dir value that is not an absolute path.
	[[nodiscard]] Status ApplyEngineCommandLine(const CommandLine& commandLine, ApplicationSpecification& specification);

	// The base of the editor and runtime applications. Not copyable or movable.
	//
	// Run, on the main thread, with the ProcessContext alive (asserted):
	//   1. Per-context initialization (§4.1 level 2): EngineContext::Create with the main window, the worker count and
	//      the ProcessContext's user-data folder as user://; a clock from the specification (SystemClock, or ManualClock
	//      with the loop's FixedDelta); then OnInitialize. A failure is logged at Error level, whatever was built is torn
	//      down in reverse, and Run returns ExitCode::InitFailed.
	//   2. The frame loop (FrameLoop; FrameLoopSpecification::ThrottleToFixedHz for headless runs with ThrottleHeadless),
	//      until RequestExit, an unhandled window close (ExitCode::Success) or MaxFrames (ExitCode::Success). The hooks
	//      below run inside it.
	//   3. OnShutdown, then the context is destroyed (reverse order). Run returns the exit code.
	// Run may be called once per Application.
	//
	// Rendering hooks (OnRender with the renderer's RenderContext, OnImGuiRender) arrive with the Graphics milestone, which
	// adds the renderer to the frame.
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
		// The process's context, for what an application adds at process level: its fatal-error hook
		// (ProcessContext::SetFatalErrorHook, the editor's autosave) and the user-data folders. Only while Run runs, from the
		// start of initialization to the end of OnShutdown (asserted).
		[[nodiscard]] ProcessContext& GetProcessContext();
		// After the context exists and before the first frame. An error makes Run return ExitCode::InitFailed.
		[[nodiscard]] virtual Status OnInitialize() { return {}; }
		// After the last frame, while the context still exists. Runs when OnInitialize succeeded.
		virtual void OnShutdown() {}
		// Every window or injected event, before InputState sees it; mark it handled to keep it from the game.
		virtual void OnEvent(Event& /*event*/) {}
		// Zero or more times per frame, one per fixed step (§4.2).
		virtual void OnFixedStep(const SimStep& /*step*/) {}
		// Once per frame after the steps; frame.Alpha is this frame's interpolation factor (§5.2).
		virtual void OnUpdate(const FrameTime& /*frame*/) {}
	private:
		void OnFrameEvent(Event& event) override;
		void OnFrameFixedStep(const SimStep& step) override;
		void OnFrameUpdate(const FrameTime& frame) override;
	private:
		ApplicationSpecification m_Specification;
		Scope<EngineContext> m_Context; // between the start of initialization and the end of OnShutdown
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
