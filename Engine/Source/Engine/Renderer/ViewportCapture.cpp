#include "EnginePCH.h"
#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"

namespace Engine {

	ViewportCapture::ViewportCapture(ConstructionKey /*key*/)
	{
	}

	ViewportCapture::~ViewportCapture() = default;

	Result<Scope<ViewportCapture>> ViewportCapture::CreateForScenes(GraphicsDevice& device, const SceneRendererPipelines& pipelines,
		GpuResourceCache& cache, AssetManager& assets)
	{
		ENGINE_TRY_ASSIGN(Scope<SceneRenderer> renderer, SceneRenderer::Create(device, pipelines, cache, assets, { .Width = 1, .Height = 1 }));
		// Not an immediate command list: NVRHI's validation allows one open immediate list at a time, and a capture may be
		// requested while the caller's frame has its own list open.
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
			device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));

		Scope<ViewportCapture> capture = CreateScope<ViewportCapture>(ConstructionKey());
		capture->m_Device = &device;
		capture->m_SceneRenderer = std::move(renderer);
		capture->m_Readback = CreateScope<Readback>(device);
		capture->m_CommandList = std::move(commandList);
		return capture;
	}

	Result<Image> ViewportCapture::Capture(const ViewportScreenshotRequest& request, const RenderSnapshot& snapshot)
	{
		const auto isValidSide = [](uint32_t side)
		{
			return side > 0 && side <= MaxViewportScreenshotDimension;
		};
		if (!isValidSide(request.Width) || !isValidSide(request.Height))
		{
			return MakeError(ErrorCode::InvalidArgument, "a viewport screenshot needs a size from 1x1 to {}x{}, got {}x{}",
				MaxViewportScreenshotDimension, MaxViewportScreenshotDimension, request.Width, request.Height);
		}

		ENGINE_TRY(m_SceneRenderer->Resize(request.Width, request.Height));
		m_CommandList->open();
		// The list is executed whatever Render reports (an error names a skipped draw; the rest rendered).
		const Status rendered = m_SceneRenderer->Render(*m_CommandList, snapshot);
		m_CommandList->close();
		m_Device->ExecuteCommandList(*m_CommandList);
		ENGINE_TRY(rendered);

		ENGINE_TRY_ASSIGN(Image image, m_Readback->ReadTexture(*m_SceneRenderer->GetFinalTexture()));
		if (request.MaxDimension > 0)
			return DownscaleImage(image, request.MaxDimension);
		return image;
	}

}
