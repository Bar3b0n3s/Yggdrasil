#pragma once

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Core/Base.h"

#include <glm/glm.hpp>

namespace Engine {

	// Registry name "Environment" (Architecture §5.3, §8.6): the image-based lighting and skybox. Unique per scene.
	// Rotation in degrees about +Y; SkyboxBlur 0-1; FallbackColor is the ambient colour when no map is assigned.
	struct EnvironmentComponent
	{
		TypedAssetHandle<AssetType::Environment> Environment;
		float Intensity = 1.0f;
		float Rotation = 0.0f;
		bool ShowSkybox = true;
		float SkyboxBlur = 0.0f;
		glm::vec3 FallbackColor = glm::vec3(0.2f, 0.22f, 0.25f); // linear
	};

}
