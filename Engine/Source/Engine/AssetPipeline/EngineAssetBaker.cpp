#include "EnginePCH.h"
#include "Engine/AssetPipeline/EngineAssetBaker.h"

// M6 contract stub (Roadmap rule 3): stream D (caches, pak, PakMount) implements the engine cooked cache.

namespace Engine {

	Result<EngineBakeReport> BakeEngineAssets(const EngineBakeSpecification& /*specification*/, const BuiltinAssetCatalog& /*catalog*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "BakeEngineAssets is an M6 contract stub");
	}

	Result<std::vector<Buffer>> GetOrBakeEngineAsset(const EngineBakeSpecification& /*specification*/, const BuiltinAssetEntry& /*entry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GetOrBakeEngineAsset is an M6 contract stub");
	}

}
