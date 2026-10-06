#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <cstdint>

// Screenshots of an application's ImGui UI (Architecture §8.13, §13.5 editor.screenshot): the last UI frame re-rendered on
// demand into an OffscreenTarget and read back (Readback), never grabbed from the last presented image. In a headless
// editor the UI runs on GLFW's null platform, so the image is the full editor as a user would see it (§13.9).
//
// It lives in Engine, beside ImGuiLayer, because its users cannot include the Editor executable's sources: the
// editor.screenshot handler belongs to EditorCore, which may not include ImGui (§3), so the Editor injects a callback over
// this function into it when the method is registered after the M4/M5 merge (Docs/Decisions/0009-m5-decisions.md). The
// Editor's --editor-screenshot option (Editor/EditorApp.h) calls it directly.

namespace Engine {

	class GraphicsDevice;
	class ImGuiLayer;

	struct ImGuiScreenshotRequest
	{
		// The larger side of the returned image at most this many pixels (DownscaleImage); 0 keeps the rendered size.
		uint32_t MaxDimension = 0;
	};

	// Renders the draw data of `imgui`'s last frame (ImGuiLayer::GetDrawData, recorded again with ImGuiLayer::Render) into a
	// new OffscreenTarget of the UI's framebuffer size (DisplaySize times FramebufferScale; RGBA8_UNORM, cleared to
	// FrameClearColor, Graphics/RenderContext.h), submits it, reads it back (Readback::ReadTexture, which waits for the copy
	// with WaitForSubmission) and applies MaxDimension. Blocks until the image is on the CPU. Main thread, between UI frames
	// (not between ImGuiLayer::BeginFrame and EndFrame). Errors: InvalidState before the first UI frame; those of
	// OffscreenTarget::Create, ImGuiLayer::Render, Readback::ReadTexture and DownscaleImage.
	[[nodiscard]] Result<Image> CaptureImGuiScreenshot(GraphicsDevice& device, ImGuiLayer& imgui, const ImGuiScreenshotRequest& request);

}
