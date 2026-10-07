#include "EnginePCH.h"
#include "Engine/Scene/PrefabAsset.h"

// M6 contract stub (Roadmap rule 3): stream E (commands, methods, hot reload) implements prefab instantiation by handle.

namespace Engine {

	Result<Prefab> LoadPrefabAsset(AssetManager& /*assets*/, AssetHandle /*handle*/, const TypeRegistry& /*registry*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "LoadPrefabAsset is an M6 contract stub");
	}

	Result<Entity> InstantiatePrefabAsset(Scene& /*scene*/, AssetManager& /*assets*/, const PrefabInstantiateOptions& /*instance*/,
		const PrefabOptions& /*options*/, LoadReport& /*report*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "InstantiatePrefabAsset is an M6 contract stub");
	}

}
