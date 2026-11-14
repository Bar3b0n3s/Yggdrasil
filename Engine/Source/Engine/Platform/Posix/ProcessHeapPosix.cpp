#include "EnginePCH.h"
#include "Engine/Platform/ProcessHeap.h"

// The C runtime's heap on Linux and macOS: no backend (ADR 0028 decision 10, Windows backends only).

#if defined(ENGINE_PLATFORM_LINUX) || defined(ENGINE_PLATFORM_MACOS)

namespace Engine {

	Result<uint64_t> ProcessHeap::GetCommittedBytes()
	{
		return MakeError(ErrorCode::Unsupported, "the C runtime heap's committed bytes have no POSIX backend (ADR 0028 decision 10)");
	}

}

#endif
