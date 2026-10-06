#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiScreenshot.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/ImGui/ImGuiLayer.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the capture
// through OffscreenTarget, ImGuiLayer::Render, Readback and DownscaleImage.

namespace Engine {

	Result<Image> CaptureImGuiScreenshot(GraphicsDevice& /*device*/, ImGuiLayer& /*imgui*/, const ImGuiScreenshotRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CaptureImGuiScreenshot is not implemented yet");
	}

}
