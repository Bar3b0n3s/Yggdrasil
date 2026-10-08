#include "EnginePCH.h"
#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/TrianglePass.h"

namespace Engine {

	namespace Utils {

		// Viewport screenshots hold display-encoded values (§8.9).
		constexpr nvrhi::Format ViewportCaptureFormat = nvrhi::Format::RGBA8_UNORM;

	}

	ViewportCapture::ViewportCapture(ConstructionKey /*key*/)
	{
	}

	ViewportCapture::~ViewportCapture() = default;

	Result<Scope<ViewportCapture>> ViewportCapture::Create(GraphicsDevice& device, PipelineFactory& pipelines)
	{
		TrianglePassSpecification specification;
		specification.Framebuffer.addColorFormat(Utils::ViewportCaptureFormat);
		specification.CullMode = nvrhi::RasterCullMode::Back;
		ENGINE_TRY_ASSIGN(Scope<TrianglePass> trianglePass, TrianglePass::Create(device, pipelines, specification));
		// Not an immediate command list: NVRHI's validation allows one open immediate list at a time, and a capture may be
		// requested while the caller's frame has its own list open.
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList,
			device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));

		Scope<ViewportCapture> capture = CreateScope<ViewportCapture>(ConstructionKey());
		capture->m_Device = &device;
		capture->m_TrianglePass = std::move(trianglePass);
		capture->m_Readback = CreateScope<Readback>(device);
		capture->m_CommandList = std::move(commandList);
		return capture;
	}

	Result<Image> ViewportCapture::Capture(const ViewportScreenshotRequest& request)
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

		// A new target per capture: the size is the request's, and nothing else renders into it.
		OffscreenTargetSpecification targetSpecification;
		targetSpecification.Width = request.Width;
		targetSpecification.Height = request.Height;
		targetSpecification.ColorFormat = Utils::ViewportCaptureFormat;
		targetSpecification.ClearColor = nvrhi::Color(TriangleClearColor[0], TriangleClearColor[1], TriangleClearColor[2], TriangleClearColor[3]);
		targetSpecification.DebugName = "ViewportCapture";
		ENGINE_TRY_ASSIGN(const OffscreenTarget target, OffscreenTarget::Create(*m_Device, targetSpecification));

		m_CommandList->open();
		m_TrianglePass->Render(*m_CommandList, *target.GetFramebuffer());
		m_CommandList->close();
		m_Device->ExecuteCommandList(*m_CommandList);

		ENGINE_TRY_ASSIGN(Image image, m_Readback->ReadTexture(*target.GetColorTexture()));
		if (request.MaxDimension > 0)
			return DownscaleImage(image, request.MaxDimension);
		return image;
	}

	Result<Scope<ViewportCapture>> ViewportCapture::CreateForScenes(GraphicsDevice& /*device*/, const SceneRendererPipelines& /*pipelines*/,
		GpuResourceCache& /*cache*/, AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "scene captures are not implemented yet (M7 stream B)");
	}

	Result<Image> ViewportCapture::Capture(const ViewportScreenshotRequest& /*request*/, const RenderSnapshot& /*snapshot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "scene captures are not implemented yet (M7 stream B)");
	}

}
