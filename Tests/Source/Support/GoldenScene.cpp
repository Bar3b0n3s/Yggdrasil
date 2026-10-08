#include "TestsPCH.h"
#include "Support/GoldenScene.h"

#include "Engine/Core/Error.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	namespace Test {

		Result<Image> RenderGoldenScene(HeadlessGpuFixture& /*gpu*/, std::string_view /*name*/, const GoldenSceneOptions& /*options*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "the golden scenes are not implemented yet (M8 stream E)");
		}

		Result<Image> ComposeImageGrid(std::span<const Image> /*images*/, uint32_t /*columns*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "the image grid is not implemented yet (M8 stream E)");
		}

	}

}
