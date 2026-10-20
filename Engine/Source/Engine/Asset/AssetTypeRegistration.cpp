#include "EnginePCH.h"
#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/SpriteSheetData.h"

namespace Engine {

	void RegisterAssetTypes(TypeRegistry& registry)
	{
		RegisterMaterialTypes(registry);
		// M18 (Docs/Decisions/0029-m18-contract.md decision 15).
		RegisterSpriteSheetTypes(registry);
	}

}
