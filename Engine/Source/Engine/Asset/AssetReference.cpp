#include "EnginePCH.h"
#include "Engine/Asset/AssetReference.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the reference syntax.

namespace Engine {

	Result<AssetReference> ParseAssetReference(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ParseAssetReference is an M6 contract stub");
	}

	std::string FormatAssetReference(const AssetReference& /*reference*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
