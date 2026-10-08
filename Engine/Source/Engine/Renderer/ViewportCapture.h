#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>

// Viewport screenshots (Architecture §8.13, §13.5 viewport.screenshot): a view re-rendered on demand at the requested size
// and read back (Readback), never grabbed from the last presented image. Its users sit above Renderer and share this one
// path: the viewport.screenshot handler (Engine/Automation/Methods/ScreenshotMethods.h), which the Editor serves through the
// capture it injects into its automation server (ScreenshotCaptures::View, EditorCore/Automation/ScreenshotMethods.h) and
// the Runtime through its own; Runtime --screenshot-at; and the Editor's --viewport-screenshot option (Editor/EditorApp.h).

namespace Engine {

	class AssetManager;
	class GpuResourceCache;
	class GraphicsDevice;
	class Readback;
	class SceneRenderer;
	class SceneRendererPipelines;
	struct RenderSnapshot;

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

	// Renders and reads back views. Created once at startup, next to the application's other GPU objects, because every
	// engine pipeline is created at startup (§8.12): the capture renders RenderSnapshots through its own SceneRenderer over
	// the host's shared SceneRendererPipelines (Docs/Decisions/0012-m7-decisions.md decisions 7 and 9), so it creates no
	// pipeline, and each Capture only resizes the renderer's targets to the requested size. The viewport.screenshot handler
	// extracts the snapshot of the requested view, camera and size (Scene/RenderExtraction.h, PlaySession::ExtractView), and
	// the Runtime's --screenshot-at does the same with its game view.
	// Not copyable or movable; main thread only (§4.11).
	class ViewportCapture
	{
	public:
		// Restricts construction to CreateForScenes; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ViewportCapture;
		};

		// Use CreateForScenes.
		explicit ViewportCapture(ConstructionKey key);
		~ViewportCapture();

		ViewportCapture(const ViewportCapture&) = delete;
		ViewportCapture& operator=(const ViewportCapture&) = delete;

		// Creates the capture's SceneRenderer over the host's `pipelines` (at 1 x 1; Capture resizes it; no pipeline is
		// created), its command list and its Readback. `device`, `pipelines`, `cache` and `assets` are documented
		// back-references that outlive the capture. Errors: those of SceneRenderer::Create (a Gpu error is an
		// out-of-memory creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<ViewportCapture>> CreateForScenes(GraphicsDevice& device, const SceneRendererPipelines& pipelines,
			GpuResourceCache& cache, AssetManager& assets);

		// Renders `snapshot` (extracted for request.Width x request.Height) through the capture's SceneRenderer resized to the
		// request's size into its own command list, submits it, reads LdrColor back (RGBA8_UNORM, display-encoded values,
		// §8.9; Readback::ReadTexture, which waits for the copy with WaitForSubmission) and applies MaxDimension. Blocks until
		// the image is on the CPU, so it serves screenshots and tests, never per-frame work. Errors: InvalidArgument for a zero
		// Width or Height or one above MaxViewportScreenshotDimension; those of SceneRenderer::Resize and Render,
		// Readback::ReadTexture and DownscaleImage.
		[[nodiscard]] Result<Image> Capture(const ViewportScreenshotRequest& request, const RenderSnapshot& snapshot);
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference
		Scope<SceneRenderer> m_SceneRenderer;
		Scope<Readback> m_Readback;
		nvrhi::CommandListHandle m_CommandList;
	};

}
