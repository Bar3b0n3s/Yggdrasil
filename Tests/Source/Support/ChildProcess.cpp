#include "TestsPCH.h"
#include "Support/ChildProcess.h"

#include "Engine/Platform/Process.h"
#include "Support/TestOptions.h"

#include <condition_variable>
#include <mutex>

// The death-test spawner on top of Platform::Process (Docs/Decisions/0003-m1-contract-decisions.md, decision 1; ADR 0005
// decision 18). The OS code it carried in M1 now lives in Platform/Windows/ProcessWindows.cpp and
// Platform/Posix/ProcessPosix.cpp.

namespace Engine {

	namespace Test {

		Result<ChildProcessResult> RunChildProcess(const std::filesystem::path& executable, std::span<const std::string> arguments,
			std::chrono::milliseconds timeout)
		{
			ProcessSpecification specification;
			specification.Executable = executable;
			specification.Arguments.assign(arguments.begin(), arguments.end());
			specification.Arguments.emplace_back(ChildProcessOption);
			ENGINE_TRY_ASSIGN(ProcessResult result, Process::Run(specification, timeout));

			ChildProcessResult child;
			child.ExitCode = result.ExitCode;
			child.StandardOutput = std::move(result.StandardOutput);
			child.StandardError = std::move(result.StandardError);
			return child;
		}

		Result<std::filesystem::path> GetCurrentExecutablePath()
		{
			return Process::GetCurrentExecutablePath();
		}

		void BlockUntilKilled()
		{
			// Nothing ever notifies, and the predicate keeps a spurious wake-up waiting.
			std::mutex mutex;
			std::condition_variable never;
			std::unique_lock lock(mutex);
			never.wait(lock, []()
			{
				return false;
			});
		}

	}

}
