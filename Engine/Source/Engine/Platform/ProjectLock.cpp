#include "EnginePCH.h"
#include "Engine/Platform/ProjectLock.h"

// M2 contract stub (Roadmap rule 3): stream B (process, crash handler, paths, project lock) implements reading the pid
// text, which needs no OS lock API. Acquire, IsHeld and the lock object live in Platform/Windows/ProjectLockWindows.cpp
// and Platform/Posix/ProjectLockPosix.cpp.

namespace Engine {

	Result<uint32_t> ProjectLock::ReadHolderPid(const std::filesystem::path& /*lockFile*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ProjectLock::ReadHolderPid is not implemented yet");
	}

}
