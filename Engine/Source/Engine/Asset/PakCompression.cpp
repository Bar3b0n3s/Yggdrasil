#include "EnginePCH.h"
#include "Engine/Asset/PakCompression.h"

#include <format>

namespace Engine {

	Result<Buffer> DeflatePakEntry(std::span<const std::byte> bytes)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("deflating a pak entry of {} bytes (Docs/Decisions/0030-m19-contract.md decision 17, stream G)", bytes.size())));
	}

	Result<Buffer> InflatePakEntry(std::span<const std::byte> stored, uint64_t size)
	{
		ENGINE_CONTRACT_STUB();
		return std::unexpected(Error(ErrorCode::Unsupported,
			std::format("inflating {} bytes of a pak entry into {} (Docs/Decisions/0030-m19-contract.md decision 17, stream G)", stored.size(), size)));
	}

}
