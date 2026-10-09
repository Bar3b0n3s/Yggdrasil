#pragma once

#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <cstdint>
#include <functional>
#include <string>

// The editor's screenshot methods (Architecture §8.13, §13.4 "Screenshots", §13.5): editor.screenshot, and the captures
// behind both screenshot methods. viewport.screenshot is the shared handler of Engine/Automation/Methods/ScreenshotMethods.h
// (the Editor and the Runtime serve it, Docs/Decisions/0012-m7-decisions.md decision 9), which renders through
// AutomationMethodContext::CaptureView; the editor's context implements that over ScreenshotCaptures::View. Conventions as
// in MethodRegistry.h.
//
// The images come from captures the editor injects through AutomationServerSpecification::Screenshots, because the
// capture of the editor UI is ImGui's (ImGui/ImGuiScreenshot.h), which EditorCore may not include (§3), and the views'
// is the Editor's ViewportCapture over its scene renderer pipelines (Renderer/ViewportCapture.h). Without them
// (--renderer none, §13.9) both methods are Unsupported. Every screenshot is re-rendered for the call, never taken from
// the last presented image (§8.13), downscaled so that its larger side is at most maxDimension (default 1024, §13.4),
// encoded as PNG and written to the server's output directory (AutomationServer::WriteOutputFile:
// project://Library/Automation/Out/, or user://Automation/Out/ without a writable project); the result names the file's
// absolute path, which the MCP bridge returns as image content (§13.8).
//
// editor.screenshot re-records the draw data of the last UI frame the editor rendered (CaptureImGuiScreenshot) over the
// frame's clear colour, so it shows the editor UI as of that frame, without the view the editor draws under it until the
// viewport panels (M10): a request answered at the safe point after another one sees that request's effects, since a UI
// frame renders in between, but a request that changed something earlier in the same AutomationServer::Pump (pipelined,
// or from another client) does not show yet, and while a windowed editor is minimized no UI frame renders, so the
// screenshot shows the UI from before it was minimized. The M5 UI (Dear ImGui's demo window) shows no editor state;
// waiting for a UI frame rendered after the request (a pending operation, which keeps the wire contract) comes with the
// editor panels (M10).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;
	struct RenderSnapshot;
	struct ViewportScreenshotRequest;

	// The captures behind the screenshot methods, injected by the editor (EditorApp) when it has a GraphicsDevice. Each
	// blocks until the image is on the CPU and returns it at its rendered size in RGBA8_UNORM; the handler downscales it.
	// Called on the main thread at the frame's safe point, between UI frames. Empty functions (the default, and with
	// --renderer none) make the methods Unsupported.
	struct ScreenshotCaptures
	{
		// `snapshot` (extracted for request.Width x request.Height) rendered by the scene renderer of the editor's
		// ViewportCapture and read back at full size (AutomationMethodContext::CaptureView, viewport.screenshot).
		std::function<Result<Image>(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)> View{};
		// The editor UI's last frame re-rendered at the UI's framebuffer size (CaptureImGuiScreenshot): the editor UI as a
		// user would see it over the frame's clear colour, on the GLFW null platform when headless (§13.9).
		std::function<Result<Image>()> EditorUi{};
		// M10 pending screenshot boundary. A request remembers the current completed serial and requests a new UI frame;
		// capture is allowed only after a later serial. Host services this while minimized, without a swapchain acquire.
		// Empty until the panels integrate; legacy synchronous capture remains available to pre-M10 callers.
		std::function<uint64_t()> CompletedUiFrame{};
		std::function<void()> RequestUiFrame{};
	};

	// viewport.screenshot's params and result (ViewportView, ViewportScreenshotParams, ViewportScreenshotResult) and
	// MaxInlineScreenshotPngBytes live in Engine/Automation/Methods/ScreenshotMethods.h since the M7 contract, because the
	// Editor and the Runtime share the method (ADR 0008 decision 26).

	// editor.screenshot {maxDimension?} (§13.5): the whole editor UI, its last UI frame re-recorded (see the file comment).
	struct EditorScreenshotParams
	{
		uint32_t MaxDimension = 1024; // 1 to MaxViewportScreenshotDimension
	};

	struct EditorScreenshotResult
	{
		std::string Path{};                 // the PNG's absolute native path
		std::string MimeType = "image/png"; // "mimeType"
		uint32_t Width = 0;                 // the PNG's size, after maxDimension
		uint32_t Height = 0;
	};

	namespace Automation {

		// editor.screenshot. Errors: Unsupported without an editor UI capture (--renderer none); the capture's errors (InvalidState
		// before the first UI frame) with the context "while rendering the editor UI"; those of DownscaleImage, EncodePng and
		// AutomationServer::WriteOutputFile.
		[[nodiscard]] Result<EditorScreenshotResult> EditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params);
		// M10 pending entry point, same wire params/result. Unsupported without the three UI callbacks. Poll until a fresh
		// frame, then use the existing encoder/output path. Cancel releases the request; timeout is the normal dispatcher
		// deadline. Integration switches RegisterScreenshotMethods to AddPending without changing the method's wire flags.
		[[nodiscard]] Result<Scope<PendingOperation>> BeginEditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params);

	}

	// Registers editor.screenshot's params and result (viewport.screenshot's are the shared
	// RegisterViewportScreenshotMethodTypes', registered through RegisterSharedMethodTypes).
	void RegisterScreenshotMethodTypes(TypeRegistry& registry);

	// Registers editor.screenshot: a read-only tool (§13.8), not available in the launcher state, no dry run (it renders, it
	// changes nothing) and not a batch op (it writes a file outside a command). viewport.screenshot is registered by the
	// shared RegisterViewportScreenshotMethods (RegisterSharedMethods).
	void RegisterScreenshotMethods(MethodRegistry& methods);

}
