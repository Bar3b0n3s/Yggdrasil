#include "EnginePCH.h"
#include "Engine/Asset/CookedFormat.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the cooked header.

namespace Engine {

	Buffer WriteCookedArtifact(AssetType /*type*/, uint16_t /*formatVersion*/, uint32_t /*importerVersion*/, std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ReadCookedArtifact is an M6 contract stub");
	}

	Result<CookedArtifactView> ReadCookedArtifact(std::span<const std::byte> /*bytes*/, AssetType /*expectedType*/, uint16_t /*formatVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ReadCookedArtifact is an M6 contract stub");
	}

}
