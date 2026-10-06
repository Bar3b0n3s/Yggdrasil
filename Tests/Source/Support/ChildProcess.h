#pragma once

#include "Engine/Core/Result.h"

#include <chrono>
#include <filesystem>
#include <span>
#include <string>

// Spawning a child process for death tests (Architecture §4.5). Platform::Process lands in M2; until then this Tests
// helper is the one place in the Tests project with OS process code (Docs/Decisions/0003-m1-contract-decisions.md).
// M2 re-implements RunChildProcess on Platform::Process behind the same declaration and removes the OS code.

namespace Engine {

	namespace Test {

		struct ChildProcessResult
		{
			// The child's exit code; on POSIX a child terminated by a signal reports 128 + the signal number.
			int ExitCode = 0;
			std::string StandardOutput; // everything the child wrote, in full
			std::string StandardError;
		};

		// Runs `executable`, a Tests executable, with `arguments` passed verbatim (no shell, no quoting rules apply) followed
		// by ChildProcessOption ("--child-process", TestOptions.h), which marks the child so that it writes neither the
		// parent's Tests.log nor crash reports into the user's folder. Runs in the current working directory, with an empty
		// standard input, capturing standard output and standard error completely (read concurrently, so a chatty child
		// cannot block). Waits at most `timeout`; on expiry the child is killed and the call fails with Timeout. Errors:
		// NotFound when `executable` does not exist, Io when it cannot be started, Timeout. Thread-safe.
		[[nodiscard]] Result<ChildProcessResult> RunChildProcess(const std::filesystem::path& executable,
			std::span<const std::string> arguments, std::chrono::milliseconds timeout);

		// The absolute path of the running executable as the OS reports it (GetModuleFileNameW on Windows,
		// /proc/self/exe on Linux, _NSGetExecutablePath on macOS, with symbolic links resolved), however the process was
		// started: argv[0] may be a bare name found through PATH, lack ".exe" or be anything the parent chose. Errors: Io.
		// M2 moves it onto Platform together with RunChildProcess.
		[[nodiscard]] Result<std::filesystem::path> GetCurrentExecutablePath();

	}

}
