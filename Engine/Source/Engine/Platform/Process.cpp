#include "EnginePCH.h"
#include "Engine/Platform/Process.h"

#include "Engine/Platform/Private/PathsUtf8.h"

// The host-independent part of Process: Run on top of Spawn, Wait and Kill. The OS-specific members live in
// Platform/Windows/ProcessWindows.cpp and Platform/Posix/ProcessPosix.cpp.

namespace Engine {

	Result<ProcessResult> Process::Run(const ProcessSpecification& specification, std::chrono::milliseconds timeout)
	{
		ENGINE_TRY_ASSIGN(Process child, Spawn(specification));
		Result<ProcessResult> result = child.Wait(timeout);
		if (result.has_value() || result.error().GetCode() != ErrorCode::Timeout)
			return result;
		// The child exited, but a process it started still holds its output: Wait's message says so, and there is nothing
		// to kill (the destructor stops reading).
		if (child.HasExited())
			return result;

		std::string message = std::format("'{}' ran longer than {} ms and was killed", Utils::NativePathToUtf8(specification.Executable),
			timeout.count());
		const Status killed = child.Kill();
		if (!killed.has_value())
			message = std::format("{}, but killing it failed: {}", result.error().GetMessageText(), killed.error().GetMessageText());
		return std::unexpected(Error(ErrorCode::Timeout, std::move(message)));
	}

}
