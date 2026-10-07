#include "EnginePCH.h"
#include "Engine/Asset/AssetTypeRegistration.h"

#include "Engine/Asset/MaterialData.h"

namespace Engine {

	void RegisterAssetTypes(TypeRegistry& registry)
	{
		RegisterMaterialTypes(registry);
	}

}
