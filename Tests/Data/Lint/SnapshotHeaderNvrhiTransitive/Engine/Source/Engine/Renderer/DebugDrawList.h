#pragma once

#include "Engine/Asset/TextureData.h"

#include <vector>

namespace Engine {

	// Asset is an allowed module, but the header it includes brings NVRHI into the snapshot headers.
	struct DebugDrawList
	{
		std::vector<TextureData> Textures;
	};

}
