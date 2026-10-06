#include "EnginePCH.h"
#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the capture
// over TrianglePass, OffscreenTarget, Readback and DownscaleImage. Until then Create fails with Unsupported.

namespace Engine {

	ViewportCapture::ViewportCapture(ConstructionKey /*key*/)
	{
	}

	ViewportCapture::~ViewportCapture() = default;

	Result<Scope<ViewportCapture>> ViewportCapture::Create(GraphicsDevice& /*device*/, PipelineFactory& /*pipelines*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ViewportCapture::Create is not implemented yet");
	}

	Result<Image> ViewportCapture::Capture(const ViewportScreenshotRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ViewportCapture::Capture is not implemented yet");
	}

}
