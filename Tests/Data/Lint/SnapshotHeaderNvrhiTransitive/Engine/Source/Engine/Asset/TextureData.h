#pragma once

#include <nvrhi/nvrhi.h>

namespace Engine {

	// Seeded defect: Asset holds CPU data only, yet this header exposes an NVRHI handle.
	struct TextureData
	{
		nvrhi::TextureHandle Texture;
	};

}
