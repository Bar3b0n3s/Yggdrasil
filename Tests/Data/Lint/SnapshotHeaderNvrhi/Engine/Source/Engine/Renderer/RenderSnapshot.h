#pragma once

#include <nvrhi/nvrhi.h>

#include <vector>

namespace Engine {

	// Seeded defect: the snapshot header that Scene may include pulls in NVRHI.
	struct RenderSnapshot
	{
		std::vector<nvrhi::TextureHandle> Textures;
	};

}
