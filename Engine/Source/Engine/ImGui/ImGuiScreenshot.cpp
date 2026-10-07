#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiScreenshot.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"

#include <imgui.h>

namespace Engine {

	Result<Image> CaptureImGuiScreenshot(GraphicsDevice& device, ImGuiLayer& imgui, const ImGuiScreenshotRequest& request)
	{
		const ImDrawData* drawData = imgui.GetDrawData();
		if (drawData == nullptr)
			return MakeError(ErrorCode::InvalidState, "there is no UI frame to capture yet");

		// The framebuffer size the UI was laid out for, truncated as Dear ImGui's renderer backends do.
		const float framebufferWidth = drawData->DisplaySize.x * drawData->FramebufferScale.x;
		const float framebufferHeight = drawData->DisplaySize.y * drawData->FramebufferScale.y;
		if (!(framebufferWidth >= 1.0f) || !(framebufferHeight >= 1.0f))
		{
			return MakeError(ErrorCode::InvalidState, "the last UI frame has no area to capture ({}x{} pixels)", framebufferWidth,
				framebufferHeight);
		}

		OffscreenTargetSpecification targetSpecification;
		targetSpecification.Width = static_cast<uint32_t>(framebufferWidth);
		targetSpecification.Height = static_cast<uint32_t>(framebufferHeight);
		targetSpecification.ColorFormat = nvrhi::Format::RGBA8_UNORM;
		targetSpecification.ClearColor = nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]);
		targetSpecification.DebugName = "ImGuiScreenshot";
		ENGINE_TRY_ASSIGN(const OffscreenTarget target, OffscreenTarget::Create(device, targetSpecification));

		// Not an immediate command list: NVRHI's validation allows one open immediate list at a time.
		ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
			device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		commandList->open();
		target.Clear(*commandList);
		const Status rendered = imgui.Render(*commandList, *target.GetFramebuffer());
		commandList->close();
		if (!rendered.has_value())
			return std::unexpected(rendered.error());
		device.ExecuteCommandList(*commandList);

		Readback readback(device);
		ENGINE_TRY_ASSIGN(Image image, readback.ReadTexture(*target.GetColorTexture()));
		if (request.MaxDimension > 0)
			return DownscaleImage(image, request.MaxDimension);
		return image;
	}

}
