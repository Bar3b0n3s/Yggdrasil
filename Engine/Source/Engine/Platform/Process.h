#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Child processes and queries about the current process (Architecture §4.5 death tests, §4.1 windowed children, §4.13
// the project-lock test). The OS code lives in Platform/Windows/ProcessWindows.cpp and Platform/Posix/ProcessPosix.cpp.

namespace Engine {

	enum class ProcessStream : uint8_t
	{
		StandardOutput,
		StandardError
	};

	struct ProcessSpecification
	{
		// The program to run: an absolute path, or one relative to the parent's working directory. There is no PATH search.
		std::filesystem::path Executable{};
		// argv[1..]: UTF-8, passed verbatim. No shell is involved; on Windows each argument is quoted so that the child's C
		// runtime (CommandLineToArgvW rules) reproduces it exactly, spaces, quotes and backslashes included.
		std::vector<std::string> Arguments{};
		// The child's working directory; empty: the parent's.
		std::filesystem::path WorkingDirectory{};
		// Environment variables set for the child on top of the parent's environment, in order (a later entry for the same
		// name wins); the parent's other variables are inherited unchanged. Names and values are UTF-8; an empty value sets
		// the variable to the empty string. Names compare as the host does (case-insensitively on Windows). Process tests
		// use it for the environment overrides of later milestones (ENGINE_VULKAN_LOADER=missing, ENGINE_GPU, §8.1).
		std::vector<std::pair<std::string, std::string>> Environment{};
	};

	struct ProcessResult
	{
		// The child's exit code. On POSIX a child ended by a signal reports 128 + the signal number. On Windows a child
		// ended by an unhandled exception reports the exception code (0xC0000005 for an access violation) as an int. A
		// child ended by Kill, by Run's timeout or by destroying its Process reports 137 on every host (128 + SIGKILL).
		int ExitCode = 0;
		std::string StandardOutput{}; // everything the child wrote, in full
		std::string StandardError{};
	};

	// A child process. Its standard input is the null device; its standard output and standard error are captured in
	// full by reader threads that run until the child closes them, so a child that fills one pipe while the parent waits
	// for the other can never block. The child inherits no other handle or descriptor of the parent, so pipes created
	// concurrently for another child never stay open in this one.
	//
	// A movable value; a moved-from Process may only be destroyed or assigned to. One thread at a time per Process;
	// different Processes may be used from different threads. Destroying a Process whose child still runs kills the child
	// and waits for it, so no child outlives its Process (a killed lock holder releases its lock, §4.13).
	class Process
	{
	public:
		// Starts the child. Errors: NotFound when Executable does not exist or is a directory; InvalidArgument for an
		// argument or environment entry that is not valid UTF-8, an empty variable name, or a name or value containing NUL
		// or a name containing '='; Io when the OS cannot start it (the message names the executable and the OS error).
		[[nodiscard]] static Result<Process> Spawn(const ProcessSpecification& specification);

		// Spawn, then Wait(timeout); on Timeout a child that still runs is killed before the error is returned ("ran longer
		// than <timeout> ms and was killed"), while a child that exited but left a process it started holding its output
		// gets Wait's error, which says so. Errors: those of Spawn and Wait. Thread-safe (each call has its own child).
		[[nodiscard]] static Result<ProcessResult> Run(const ProcessSpecification& specification, std::chrono::milliseconds timeout);

		~Process();

		Process(Process&& other) noexcept;
		Process& operator=(Process&& other) noexcept;
		Process(const Process&) = delete;
		Process& operator=(const Process&) = delete;

		// The child's process ID.
		[[nodiscard]] uint32_t GetId() const;

		// True once the child has exited (does not wait).
		[[nodiscard]] bool HasExited();

		// Waits up to `timeout` for the child to exit and for both output streams to reach end of file, then returns the
		// exit code and the complete output, which moves out of the Process. Errors: Timeout when the child is still
		// running (it keeps running; Wait may be called again); InvalidState after a successful Wait; Io.
		[[nodiscard]] Result<ProcessResult> Wait(std::chrono::milliseconds timeout);

		// Waits up to `timeout` until the captured `stream` contains `text` (output written before this call counts), for
		// example a child announcing that it holds a lock. Errors: Timeout; NotFound when the stream reached end of file
		// without it (the child closed it or exited); InvalidState after a successful Wait.
		[[nodiscard]] Status WaitForOutput(ProcessStream stream, std::string_view text, std::chrono::milliseconds timeout);

		// Ends the child forcibly (TerminateProcess, SIGKILL) and waits until it is gone; the captured output stays readable
		// through Wait, whose exit code is then the forced one. Succeeds when the child had already exited. Errors: Io.
		[[nodiscard]] Status Kill();

		// The current process's ID.
		[[nodiscard]] static uint32_t GetCurrentId();

		// True while a process with ID `processId` runs (the current one included), including one this process may not
		// otherwise touch (another user's); false for an ID no process has (0, an exited process). A process ID is reused
		// after its process ended, so true can also mean a later process with the same ID; on POSIX an exited child that
		// its parent has not reaped yet still counts as running. Used to tell files of live processes from leftovers (the
		// automation server's offloaded results).
		[[nodiscard]] static bool IsRunning(uint32_t processId);

		// The absolute path of the running executable as the OS reports it (GetModuleFileNameW, /proc/self/exe,
		// _NSGetExecutablePath), with symbolic links resolved, however the process was started (argv[0] may be a bare name
		// found through PATH or anything the parent chose). Errors: Io.
		[[nodiscard]] static Result<std::filesystem::path> GetCurrentExecutablePath();

		// The CPU time the current process has used so far, user plus kernel, in seconds (GetProcessTimes, getrusage).
		// Errors: Io.
		[[nodiscard]] static Result<double> GetCurrentCpuSeconds();

		// True while a debugger is attached to the current process (IsDebuggerPresent; TracerPid in /proc/self/status on
		// Linux; P_TRACED from sysctl on macOS).
		[[nodiscard]] static bool IsDebuggerAttached();

		// Stops in the attached debugger (__debugbreak, SIGTRAP). Without a debugger this ends the process like a crash, so
		// callers check IsDebuggerAttached first (ProcessContext's fatal-error handler does, for failed assertions; ADR
		// 0003 decision 21).
		static void BreakIntoDebugger();
	private:
		struct Impl;

		explicit Process(Scope<Impl> impl);
	private:
		Scope<Impl> m_Impl;
	};

}
