#include "EnginePCH.h"
#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/StringTableData.h"
#include "Engine/Asset/TilemapData.h"

namespace Engine {

	void RegisterAssetTypes(TypeRegistry& registry)
	{
		RegisterMaterialTypes(registry);
		// M18 (Docs/Decisions/0029-m18-contract.md decision 15).
		RegisterSpriteSheetTypes(registry);
		// M21 (Docs/Decisions/0032-m21-contract.md decision 23).
		RegisterTilemapTypes(registry);
		// M22 (Docs/Decisions/0033-m22-contract.md decision 10).
		RegisterStringTableTypes(registry);
	}

}
