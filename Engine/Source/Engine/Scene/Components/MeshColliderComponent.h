#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

namespace Engine {

	// Registry name "MeshCollider" (Architecture §5.3, §9.2): a mesh shape. A null Mesh uses the entity's MeshRenderer
	// mesh. A non-convex mesh requires a Static or Kinematic body (PHYSICS_* validation, M11).
	struct MeshColliderComponent
	{
		TypedAssetHandle<AssetType::Mesh> Mesh;
		bool Convex = false;
		bool IsTrigger = false;
	};

}
