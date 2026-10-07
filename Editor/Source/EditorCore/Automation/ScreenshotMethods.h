#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

// The screenshot methods (Architecture §8.13, §13.4 "Screenshots", §13.5): viewport.screenshot and editor.screenshot,
// registered at the M4/M5 merge (Docs/Decisions/0009-m5-decisions.md decisions 16 and 33). Conventions as in
// MethodRegistry.h.
//
// The images come from captures the editor injects through AutomationServerSpecification::Screenshots, because the
// capture of the editor UI is ImGui's (ImGui/ImGuiScreenshot.h), which EditorCore may not include (§3); the viewport's is
// Renderer/ViewportCapture.h. Without them (--renderer none, §13.9) both methods are Unsupported. Every screenshot is
// re-rendered for the call, never taken from the last presented image (§8.13), downscaled so that its larger side is at
// most maxDimension (default 1024, §13.4), encoded as PNG and written to the server's output directory
// (AutomationServer::WriteOutputFile: project://Library/Automation/Out/, or user://Automation/Out/ without a writable
// project); the result names the file's absolute path, which the MCP bridge returns as image content (§13.8). A result
// stays below the offload threshold (§13.4), so its path and mimeType always reach the client: viewport.screenshot's
// inline data is left out of a PNG larger than MaxInlineScreenshotPngBytes.
//
// viewport.screenshot renders the viewport's current state. editor.screenshot re-records the draw data of the last UI
// frame the editor rendered (CaptureImGuiScreenshot), so it shows the editor as of that frame: a request answered at
// the safe point after another one sees that request's effects, since a UI frame renders in between, but a request
// that changed something earlier in the same AutomationServer::Pump (pipelined, or from another client) does not show
// yet, and while a windowed editor is minimized no UI frame renders, so the screenshot shows the UI from before it was
// minimized. The M5 UI (Dear ImGui's demo window) shows no editor state; waiting for a UI frame rendered after the
// request (a pending operation, which keeps the wire contract) comes with the editor panels (M10).

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The captures behind the screenshot methods, injected by the editor (EditorApp) when it has a GraphicsDevice. Each
	// blocks until the image is on the CPU and returns it at its rendered size in RGBA8_UNORM; the handler downscales it.
	// Called on the main thread at the frame's safe point, between UI frames. Empty functions (the default, and with
	// --renderer none) make the methods Unsupported.
	struct ScreenshotCaptures
	{
		// The viewport re-rendered at width x height, each 1 to MaxViewportScreenshotDimension (ViewportCapture::Capture: the
		// clear-and-triangle view until the scene renderer replaces it in M7).
		std::function<Result<Image>(uint32_t width, uint32_t height)> Viewport{};
		// The editor UI's last frame re-rendered at the UI's framebuffer size (CaptureImGuiScreenshot): the whole editor as
		// a user would see it, on the GLFW null platform when headless (§13.9).
		std::function<Result<Image>()> EditorUi{};
	};

	// The largest PNG viewport.screenshot returns inline (base64 "data"): 30 KB, 40 KB of base64, which leaves the result's
	// other members (the path among them) 8 KB below the 48 KB offload threshold (DefaultOffloadThresholdBytes). A larger
	// PNG is only written, and the result reports inlineOmitted.
	inline constexpr size_t MaxInlineScreenshotPngBytes = 30 * 1024;

	// Which view viewport.screenshot renders (§13.5): registry enum "ViewportView".
	enum class ViewportView : uint8_t
	{
		Scene, // the editor viewport's view of the edit scene
		Game   // the play scene through its game camera (M7)
	};

	// viewport.screenshot {view, width?, height?, camera?, debugView?, annotate?, maxDimension?, inline?} (§13.5). M5
	// renders the scene view of the clear-and-triangle viewport. The members that need later milestones are validated and
	// refused with Unsupported, located at their pointer, whenever they are present: view "Game" (play sessions and the scene
	// renderer, M7), camera (camera entities, M7), debugView (the debug views of §8.5, M8) and annotate (the overlay pass of
	// §8.13, M9). They are never ignored.
	struct ViewportScreenshotParams
	{
		ViewportView View = ViewportView::Scene; // required
		uint32_t Width = 640;                    // 1 to MaxViewportScreenshotDimension; the rendered size before maxDimension
		uint32_t Height = 360;
		std::string Camera{};         // an EntityRef of a camera entity (M7)
		std::string DebugView{};      // a debug view name (M8)
		VariantValue Annotate{};      // {labels, colliders, bounds, axes} (M9)
		uint32_t MaxDimension = 1024; // 1 to MaxViewportScreenshotDimension
		bool Inline = false;          // also return the PNG as base64 in "data", when it fits MaxInlineScreenshotPngBytes
	};

	struct ViewportScreenshotResult
	{
		ViewportView View = ViewportView::Scene;
		std::string Path{};                 // the PNG's absolute native path
		std::string MimeType = "image/png"; // "mimeType"
		uint32_t Width = 0;                 // the PNG's size, after maxDimension
		uint32_t Height = 0;
		// With inline: the PNG, base64 (RFC 4648, with padding), when it is at most MaxInlineScreenshotPngBytes; empty
		// otherwise. Inline images suit a small maxDimension.
		std::string Data{};
		// With inline and a PNG larger than MaxInlineScreenshotPngBytes: true, and Data is empty (the PNG is at Path).
		bool InlineOmitted = false;
	};

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

		// viewport.screenshot. Errors: Unsupported at the member's pointer for the members above that need later milestones,
		// and without a viewport capture (--renderer none); the capture's errors (a Gpu error is Internal) with the context
		// "while rendering the viewport"; those of DownscaleImage, EncodePng and AutomationServer::WriteOutputFile.
		[[nodiscard]] Result<ViewportScreenshotResult> ViewportScreenshot(EditorMethodContext& context, const ViewportScreenshotParams& params);
		// editor.screenshot. Errors: Unsupported without an editor UI capture (--renderer none); the capture's errors (InvalidState
		// before the first UI frame) with the context "while rendering the editor UI"; those of DownscaleImage, EncodePng and
		// AutomationServer::WriteOutputFile.
		[[nodiscard]] Result<EditorScreenshotResult> EditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params);

	}

	void RegisterScreenshotMethodTypes(TypeRegistry& registry);

	// Registers viewport.screenshot and editor.screenshot: read-only tools (§13.8), not available in the launcher state, no
	// dry run (they render, they change nothing) and not batch ops (they write a file outside a command). viewport.screenshot
	// is in the Runtime subset (§13.5, M7; its declarations move to Engine/Automation/Methods then, ADR 0008 decision 26).
	void RegisterScreenshotMethods(MethodRegistry& methods);

}
