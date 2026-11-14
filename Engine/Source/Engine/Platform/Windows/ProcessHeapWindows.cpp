#include "EnginePCH.h"
#include "Engine/Platform/ProcessHeap.h"

// The C runtime's heap on Windows: the Universal CRT allocates from the process heap (GetProcessHeap, which
// _get_heap_handle names; "ProcessHeap: the C runtime allocates from the process heap"), and HeapSummary reports its
// committed bytes, the blocks it gives VirtualAlloc'd memory of their own included.

#if defined(ENGINE_PLATFORM_WINDOWS)

	#include <windows.h>

namespace Engine {

	Result<uint64_t> ProcessHeap::GetCommittedBytes()
	{
		HEAP_SUMMARY summary{};
		summary.cb = sizeof(summary);
		if (HeapSummary(GetProcessHeap(), 0, &summary) == FALSE)
			return MakeError(ErrorCode::Io, "HeapSummary failed on the process heap (error {})", GetLastError());
		return static_cast<uint64_t>(summary.cbCommitted);
	}

}

#endif
