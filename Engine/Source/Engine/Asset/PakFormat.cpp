#include "EnginePCH.h"
#include "Engine/Asset/PakFormat.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the pak header and TOC.

namespace Engine {

	std::array<std::byte, PakHeader::Size> WritePakHeader(const PakHeader& /*header*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<PakHeader> ReadPakHeader(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ReadPakHeader is an M6 contract stub");
	}

	std::string SerializePakToc(const PakToc& /*toc*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<PakToc> ParsePakToc(std::string_view /*text*/, uint64_t /*dataEnd*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParsePakToc is an M6 contract stub");
	}

}
