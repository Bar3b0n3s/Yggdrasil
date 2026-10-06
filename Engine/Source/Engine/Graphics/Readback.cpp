#include "EnginePCH.h"
#include "Engine/Graphics/Readback.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the staging
// copy, the bounded wait and the row repacking of Architecture §8.1 "Headless".

namespace Engine {

	Readback::Readback(GraphicsDevice& /*device*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Readback::~Readback() = default;

	Result<Image> Readback::ReadTexture(nvrhi::ITexture& /*texture*/, uint32_t /*mipLevel*/, uint32_t /*arraySlice*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Readback::ReadTexture is not implemented yet");
	}

}
