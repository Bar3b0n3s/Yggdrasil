#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <vector>

// Reading a GPU buffer back to the CPU in GPU tests (M23, C23.3; Docs/Decisions/0034-m23-contract.md decision 27, stream C):
// Graphics/Readback reads textures only, and the Skinning pass's test compares the deformed vertices and the previous
// positions it wrote with the CPU reference. A test helper, not engine code, because only tests read buffers back.

namespace Engine {

	class GraphicsDevice;

	namespace Test {

		// The first `byteCount` bytes of `buffer` (at most its size), copied through a CPU-readable staging buffer on a command
		// list of its own, which `device` executes and waits for; `buffer` is in a state the copy may read (its kept state).
		// Main thread. Errors: InvalidArgument for a byte count of 0 or above the buffer's size; Gpu when the staging buffer
		// cannot be created or mapped.
		[[nodiscard]] Result<std::vector<std::byte>> ReadBufferBytes(GraphicsDevice& device, nvrhi::IBuffer& buffer, uint64_t byteCount);

	}

}
