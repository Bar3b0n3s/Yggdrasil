#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>

// The heap the C runtime allocates from (malloc, operator new, and so every first-party allocation and every library
// that allocates through the C runtime). Its committed bytes measure the process's own memory without what a graphics
// driver or another component keeps in allocations of its own (VirtualAlloc'd arenas, private heaps): the hardening
// soak's memory rule measures them through stats.get, bounding their growth by 1 % of the private bytes, so the memory
// the NVIDIA driver's one-off background shader compilation commits in heaps of its own fails no subject
// (Docs/Decisions/0036-m25-tetris-pixel-art.md, "Landing", "The soak", A-prime). Windows backend only (ADR 0028
// decision 10): HeapSummary on the process heap, the heap the Universal CRT allocates from, whose committed size includes
// its large blocks; Linux and macOS report ErrorCode::Unsupported. In the hardening stage's AddressSanitizer build the
// sanitizer's runtime serves malloc, operator new and HeapAlloc from its own allocator, so the measure leaves those
// allocations out there (the soak never runs on that build).

namespace Engine {

	class ProcessHeap
	{
	public:
		ProcessHeap() = delete;

		// The committed bytes of the C runtime's heap, its large blocks included. Thread-safe; it takes the heap's lock for
		// the summary. Errors: Unsupported on Linux and macOS; Io when the query fails.
		[[nodiscard]] static Result<uint64_t> GetCommittedBytes();
	};

}
