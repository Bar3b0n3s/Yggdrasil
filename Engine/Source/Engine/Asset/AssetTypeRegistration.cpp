#include "EnginePCH.h"
#include "Engine/Asset/AssetTypeRegistration.h"

// M6 contract stub (Roadmap rule 3): stream B (texture, material and font importers) registers the Asset module's types
// (RegisterMaterialTypes).

namespace Engine {

	void RegisterAssetTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
