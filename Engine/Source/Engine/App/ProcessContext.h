#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/RingBufferSink.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/Paths.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Process-level initialization (Architecture §4.1 level 1, §3 rule 5). GLFW, the logger registry, the profiler, the
// crash handler and later the Vulkan loader, Jolt's factory and Luau's flags are process-global, so exactly one
// ProcessContext initializes them, created first by RunApplication or the Tests main and destroyed last, after every
// EngineContext.

namespace Engine {

	// The process-level steps, in initialization order. Teardown runs them in reverse. Later milestones insert theirs at
	// the places §4.1 gives: Luau fast flags and PhysicsEngine::Initialize after CrashHandler, the Vulkan loader before Glfw.
	enum class ProcessContextStep : uint8_t
	{
		Log,          // Log::Initialize: console sink and the rotating file <UserData>/<AppName>/Logs/<exe>.log (§4.4)
		Profiler,     // Profiler::Initialize (ADR 0003 decision 6)
		CrashHandler, // CrashHandler::Install with <UserData>/<AppName>/Crashes, and the fatal-error handler (below)
		Glfw          // GlfwLibrary::Initialize with the platform chosen once by WindowMode
	};

	struct ProcessContextSpecification
	{
		// The application name: the user-data folder (§4.4), validated by Paths::ValidateAppName. ENGINE_PRODUCT_NAME for
		// the Editor and Tests, the manifest Name for exported games.
		std::string AppName{};
		// Windowed: GLFW's native platform; Headless: the null platform. Chosen once for the whole process (§4.1).
		WindowMode Window = WindowMode::Windowed;
		// The user-data root that replaces the OS's (Paths::GetUserDataRoot) when set: tests point it at a temporary
		// directory. Absolute.
		std::filesystem::path UserDataRoot{};
		// The rotating log file under <UserData>/<AppName>/Logs. Child processes whose parent captures their output turn it
		// off (the Tests main for every --child-process, Support/TestOptions.h), so that two processes never write one log
		// file.
		bool LogToFile = true;
		// The console sink (stderr; never in Dist) and its level.
		bool LogToConsole = true;
		LogLevel ConsoleLevel = LogLevel::Trace;
		// Crash reports under <UserData>/<AppName>/Crashes. Off for Tests children that were given no user-data root of
		// their own (death tests, recording-assert-handler targets), whose deaths are expected: crashes still end the process
		// with exit code 4, but no report is written into the user's folder.
		bool WriteCrashReports = true;
	};

	// An application's step in the fatal-error path (§4.6): the editor's autosave of the open scene and dirty native assets
	// (§4.13), which is pure CPU serialization and never touches the GPU. Function runs on the thread that called
	// FatalError, possibly while other threads still run, with UserData as its first argument. It must not throw, must not
	// call FatalError (a nested call skips the handler), must not wait for another thread (that thread may be the failing
	// one or hold the lock it needs) and should finish quickly: the process exits right after the handler.
	struct FatalErrorHook
	{
		void (*Function)(void* userData, FatalErrorKind kind, std::string_view message) = nullptr;
		void* UserData = nullptr; // a documented back-reference: whatever installs the hook keeps it alive until it removes it
	};

	// The process-level context. Not copyable or movable; at most one exists at a time (process-level state).
	//
	// Create runs the steps in ProcessContextStep order, each returning Status. Before the log exists it resolves the
	// user-data folders (Paths::GetUserDataPaths, Paths::CreateUserDataDirectories) and the executable's name for the log
	// file (Process::GetCurrentExecutablePath). On a failed step the completed steps are torn down in reverse and Create
	// returns the error with the step's context; RunApplication turns it into ExitCode::InitFailed. Every step logs one
	// Info line when it completes, "Process context: <Step> initialized", and one before its teardown, "Process context:
	// shutting down <Step>" (the log step's own line is the last one the log prints), so a child process's output shows
	// the order.
	//
	// The fatal-error handler (Core SetFatalErrorHandler, installed by the CrashHandler step and restored at its
	// teardown) is the only Core FatalErrorHandler of the process. It runs on the failing thread before FatalError
	// flushes the logs and exits, and does, in this order (§4.6):
	//   1. for FatalErrorKind::Assert with a debugger attached, break into it (Process::IsDebuggerAttached,
	//      Process::BreakIntoDebugger; ADR 0003 decision 21);
	//   2. the application's hook (SetFatalErrorHook) when one is set: the editor's autosave;
	//   3. the crash report (CrashHandler::WriteFatalErrorReport) when WriteCrashReports is on;
	//   4. in a windowed process, a message box naming the error (§4.6, §8.1, §14.3). The Platform call that shows it
	//      arrives with the Graphics milestone, which first needs it (no Vulkan loader or device); until then the step
	//      does nothing, but its place in the order is fixed.
	// The assert handler itself stays Core's DefaultAssertHandler, or whatever the Tests main installs.
	//
	// Main thread only, except SetFatalErrorHook (thread-safe): create and destroy the context on the process's main thread
	// (GLFW's rule), while no other thread logs.
	class ProcessContext
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ProcessContext;
		};

		// Use Create.
		ProcessContext(ConstructionKey key, ProcessContextSpecification specification);
		// Tears the completed steps down in reverse order.
		~ProcessContext();

		ProcessContext(const ProcessContext&) = delete;
		ProcessContext& operator=(const ProcessContext&) = delete;

		// Initializes the process (see the class comment). Creating a second context while one exists is a programmer
		// error (asserted with the message "a ProcessContext already exists"). Errors: those of the failed step, with the
		// step as context: Validation for a bad AppName, NotFound or Io for the user-data folders, Io for the log file,
		// Unsupported when GLFW cannot use the windowed platform.
		[[nodiscard]] static Result<Scope<ProcessContext>> Create(const ProcessContextSpecification& specification);

		// The live context; nullptr when none exists. Only the App module (RunApplication, Application::Run, which hands it
		// to the application through Application::GetProcessContext), the Tests main and tests call it. Every other piece of
		// code receives the services it needs explicitly (§4.1: there is no global way into the application or its
		// services); review enforces this.
		[[nodiscard]] static ProcessContext* GetCurrent();

		// Sets the application's fatal-error hook (step 2 of the handler, see the class comment), replacing any previous one;
		// a hook whose Function is null removes it. An application sets it once the state it saves exists and removes it
		// before that state is destroyed; the context drops it at teardown. Thread-safe: the handler reads the hook under
		// the same lock, so a hook is never called after the SetFatalErrorHook that removed it returned.
		void SetFatalErrorHook(FatalErrorHook hook);

		[[nodiscard]] const ProcessContextSpecification& GetSpecification() const { return m_Specification; }

		// The completed steps, in initialization order (all four once Create succeeded).
		[[nodiscard]] std::span<const ProcessContextStep> GetSteps() const { return m_Steps; }

		// <UserData>/<AppName> and its Logs and Crashes folders (created by Create).
		[[nodiscard]] const UserDataPaths& GetUserDataPaths() const { return m_UserDataPaths; }

		// The log file in use; empty when LogToFile is off.
		[[nodiscard]] const std::filesystem::path& GetLogFilePath() const { return m_LogFilePath; }

		// The platform GLFW runs on (GlfwLibrary::GetPlatform): Null for Headless, the native one for Windowed.
		[[nodiscard]] GlfwPlatform GetGlfwPlatform() const { return GlfwLibrary::GetPlatform(); }
	private:
		ProcessContextSpecification m_Specification;
		std::vector<ProcessContextStep> m_Steps; // completed, in order; teardown pops from the back
		UserDataPaths m_UserDataPaths;
		std::filesystem::path m_LogFilePath;
	};

	// "Log", "Profiler", "CrashHandler" or "Glfw".
	[[nodiscard]] std::string_view ProcessContextStepToString(ProcessContextStep step);

	// The build description written into crash reports and the startup log line: "<Debug|Release|Dist>
	// <windows|linux|macos>-<x86_64|arm64>".
	[[nodiscard]] std::string_view GetBuildDescription();

}
