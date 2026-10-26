#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiScreenshot.h"

#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/ImGui/ImGuiLayer.h"

#include <imgui.h>

#include <format>
#include <optional>
#include <utility>

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
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42, 44 and 45): the capture is one request. When a creation
		// runs out of memory (the target, ImGui's pipeline for its format, a geometry buffer, the staging texture), its list has
		// been executed, and the trim waits for it before the whole capture runs once more; a second failure is OutOfMemory.
		// The target lives until the capture returns, so it outlives the submission that cleared it until the trim's wait
		// (NVRHI does not reference a texture used only by a clear, ADR 0009 decision 31). A retry reuses the target an earlier
		// attempt created (each attempt clears it again), so it never needs memory for a second one while the first is held.
		std::optional<OffscreenTarget> target;
		Image image;
		const Status captured = RetryAfterTrim(device, std::format("an editor UI capture at {}x{}", targetSpecification.Width, targetSpecification.Height),
			[&device, &imgui, &targetSpecification, &target, &image]() -> Status
		{
			if (!target.has_value())
			{
				ENGINE_TRY_ASSIGN(OffscreenTarget created, OffscreenTarget::Create(device, targetSpecification));
				target = std::move(created);
			}

			// Not an immediate command list: NVRHI's validation allows one open immediate list at a time.
			ENGINE_TRY_ASSIGN(const nvrhi::CommandListHandle commandList,
				device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
			// The list is executed whatever Render reports, so a failed render leaves no recording behind (CommandListRecording).
			CommandListRecording recording(device, *commandList);
			target->Clear(*commandList);
			const Status rendered = imgui.Render(*commandList, *target->GetFramebuffer());
			recording.Execute();
			ENGINE_TRY(rendered);

			Readback readback(device);
			ENGINE_TRY_ASSIGN(image, readback.ReadTexture(*target->GetColorTexture()));
			return {};
		});
		ENGINE_TRY(captured);
		if (request.MaxDimension > 0)
			return DownscaleImage(image, request.MaxDimension);
		return image;
	}

}
