#include "EnginePCH.h"
#include "Engine/Platform/Process.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements Run on top of
// Spawn, Wait and Kill. The OS-specific members live in Platform/Windows/ProcessWindows.cpp and
// Platform/Posix/ProcessPosix.cpp.

namespace Engine {

	Result<ProcessResult> Process::Run(const ProcessSpecification& /*specification*/, std::chrono::milliseconds /*timeout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Process::Run is not implemented yet");
	}

}
