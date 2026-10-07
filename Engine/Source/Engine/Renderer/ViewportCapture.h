#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

// Viewport screenshots (Architecture §8.13, §13.5 viewport.screenshot): the viewport re-rendered on demand at the
// requested size into an OffscreenTarget and read back (Readback), never grabbed from the last presented image. Its users
// sit above Renderer and share this one path: the viewport.screenshot handler of Automation/Methods, which serves the
// Editor and the Runtime alike and is registered after the M4/M5 merge (Docs/Decisions/0009-m5-decisions.md), Runtime
// --screenshot-at (M7), and the Editor's --viewport-screenshot option (Editor/EditorApp.h).

namespace Engine {

	class GraphicsDevice;
	class PipelineFactory;
	class Readback;
	class TrianglePass;

	// The golden-image size (§15.4), the default of viewport screenshots.
	inline constexpr uint32_t DefaultViewportScreenshotWidth = 640;
	inline constexpr uint32_t DefaultViewportScreenshotHeight = 360;
	// The largest Width or Height a request may ask for.
	inline constexpr uint32_t MaxViewportScreenshotDimension = 8192;

	struct ViewportScreenshotRequest
	{
		uint32_t Width = DefaultViewportScreenshotWidth;
		uint32_t Height = DefaultViewportScreenshotHeight;
		// The larger side of the returned image at most this many pixels (DownscaleImage); 0 keeps the rendered size.
		uint32_t MaxDimension = 0;
	};

	// Renders and reads back the viewport. Created once at startup, next to the application's other GPU objects, because
	// every engine pipeline is created at startup (§8.12): the capture owns the pipeline it draws with, and each Capture
	// only creates the target of the requested size. In M5 the viewport is the clear-and-triangle view
	// (Renderer/TrianglePass); the scene renderer replaces it in M7 and the request gains the camera, view and debug view.
	// Not copyable or movable; main thread only (§4.11).
	class ViewportCapture
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ViewportCapture;
		};

		// Use Create.
		explicit ViewportCapture(ConstructionKey key);
		~ViewportCapture();

		ViewportCapture(const ViewportCapture&) = delete;
		ViewportCapture& operator=(const ViewportCapture&) = delete;

		// Creates the capture's TrianglePass for RGBA8_UNORM targets without depth (back-face culling, as the golden image
		// "Triangle" renders it) and its Readback. `device` is a documented back-reference that must outlive the capture.
		// Errors: those of TrianglePass::Create; a Gpu error is an out-of-memory pipeline creation, which a caller creating
		// the capture at startup turns into FatalError(OutOfMemory) (§8.14 item 7).
		[[nodiscard]] static Result<Scope<ViewportCapture>> Create(GraphicsDevice& device, PipelineFactory& pipelines);

		// Renders the viewport at the request's size into a new OffscreenTarget (RGBA8_UNORM, display-encoded values, §8.9),
		// submits it, reads it back (Readback::ReadTexture, which waits for the copy with WaitForSubmission) and applies
		// MaxDimension. Blocks until the image is on the CPU, so it serves screenshots and tests, never per-frame work.
		// Errors: InvalidArgument for a zero Width or Height or one above MaxViewportScreenshotDimension; those of
		// OffscreenTarget::Create, Readback::ReadTexture and DownscaleImage.
		[[nodiscard]] Result<Image> Capture(const ViewportScreenshotRequest& request);
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference
		Scope<TrianglePass> m_TrianglePass;
		Scope<Readback> m_Readback;
		nvrhi::CommandListHandle m_CommandList;
	};

}
