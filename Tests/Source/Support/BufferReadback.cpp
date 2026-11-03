#include "TestsPCH.h"
#include "Support/BufferReadback.h"

// The GPU buffer readback of the tests (BufferReadback.h). A contract stub of M23's C23.3, implemented by stream C
// (Docs/Decisions/0034-m23-contract.md decision 27): until then nothing is read back.

namespace Engine {

	namespace Test {

		Result<std::vector<std::byte>> ReadBufferBytes(GraphicsDevice& /*device*/, nvrhi::IBuffer& /*buffer*/, uint64_t /*byteCount*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "M23 skeletal animation contract (Docs/Decisions/0034-m23-contract.md decision 27, stream C)");
		}

	}

}
