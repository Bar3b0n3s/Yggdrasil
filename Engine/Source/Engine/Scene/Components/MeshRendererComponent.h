#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <vector>

namespace Engine {

	// Registry name "MeshRenderer" (Architecture §5.3): draws a mesh asset. Materials has one slot per submesh; an empty
	// list or a null slot uses the mesh's default material for that submesh.
	struct MeshRendererComponent
	{
		TypedAssetHandle<AssetType::Mesh> Mesh;
		std::vector<TypedAssetHandle<AssetType::Material>> Materials;
		bool CastShadows = true;
		bool ReceiveShadows = true;
		bool Visible = true;
	};

}
