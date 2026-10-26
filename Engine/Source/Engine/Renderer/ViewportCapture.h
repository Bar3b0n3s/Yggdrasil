#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"

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
	// pipeline, and each Capture only resizes the renderer to the requested size. The viewport.screenshot handler extracts
	// the snapshot of the requested view, camera and size (Scene/RenderExtraction.h, PlaySession::ExtractView), and the
	// Runtime's --screenshot-at does the same with its game view.
	//
	// Device loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 26, 27 and 33): CreateForScenes registers the
	// capture with its device's recreation registry as "ViewportCapture", after its private renderer, which is the placement
	// rule's place 2 and registers itself ("SceneRenderer"). The capture's Release drops its own GPU objects, the read-back
	// command list and its Readback's (a new Readback, which creates its command list on first use); its Recreate creates the
	// command list again. A capture after the recovery renders the same snapshot into the same bytes as one before the loss.
	//
	// Out-of-memory (M18, C18.4; decision 43): a capture keeps no image on display, since its last image was read back, so
	// its renderer's targets are rebuilt on demand. Capture releases them (SceneRenderer::ReleaseTargets) before it resizes
	// the renderer to another size, so it does not hold the previous size's targets while it creates the new ones (their
	// memory goes with their last reference: at once in Dist, and where the resource tracker counts at the next garbage
	// collection, which the resize's trim runs when it needs the memory). The registration's TrimUnused hook releases them
	// too between captures (a trim that another owner's request runs), so the last capture's targets never outlive a trim;
	// inside a capture, its own requests' trims leave them to that capture (the release before a new size has already
	// dropped the previous size's). The private renderer's own registration then trims its pool, which runs after the
	// capture's hook (last registered first).
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
		// created here, and a capture of a debug view extends `pipelines` through SceneRendererPipelines::EnsureDebugView),
		// its command list and its Readback, and registers the capture with the device's recreation registry (see the class
		// comment). `device`, `pipelines`, `cache` and `assets` are documented back-references that outlive the capture.
		// Errors: those of SceneRenderer::Create and GraphicsDevice::CreateCommandList (a Gpu error is an out-of-memory
		// creation, FatalError(OutOfMemory) for a caller at startup, §8.14 item 7).
		[[nodiscard]] static Result<Scope<ViewportCapture>> CreateForScenes(GraphicsDevice& device, SceneRendererPipelines& pipelines,
			GpuResourceCache& cache, AssetManager& assets);

		// Renders `snapshot` (extracted for request.Width x request.Height) through the capture's SceneRenderer resized to the
		// request's size into its own command list, submits it, reads the renderer's final image back (SceneRenderer::
		// GetFinalTexture at the request's size: the scene image, or with a snapshot whose output stage scales the scene the
		// upscaled scene image in its letterbox, M18; RGBA8_UNORM, display-encoded values, §8.9; Readback::ReadTexture, which
		// waits for the copy with WaitForSubmission) and applies MaxDimension. Blocks until the image is on the CPU, so it
		// serves screenshots and tests, never per-frame work. Errors: InvalidArgument for a zero
		// Width or Height or one above MaxViewportScreenshotDimension; those of SceneRenderer::Resize and Render,
		// Readback::ReadTexture and DownscaleImage.
		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42, 44 and 45): the capture's OutOfMemory. A capture is
		// one request: its resize, its render (a debug view's first pipelines, a lazily created target or binding set) and
		// its read-back staging texture. Before a resize to another size it releases the renderer's previous targets (see the
		// class comment). When any part runs out of memory (IsGpuOutOfMemory), the capture trims and runs the whole capture
		// once more (RetryAfterTrim; its first render is submitted and retired first, so the trim runs between recordings);
		// a second out-of-memory failure returns ErrorCode::OutOfMemory, never an image of another size or debug view, and
		// GetLastOutputMapping stays the last successful capture's; every capture is a new request that tries again. A part
		// that is a request of its own (the renderer's Resize, a debug view's first pipelines) trims and retries inside it,
		// and when it fails alone its OutOfMemory is not trimmed for again: at most one trim per RetryAfterTrim call. The
		// retried capture is byte-identical to one that met no out-of-memory. Its callers pass OutOfMemory on unchanged and
		// never end the process for it (viewport.screenshot answers it in error.data.errorCode, a thumbnail stays failed,
		// the Runtime's --screenshot-at fails its run with exit code 1); out-of-memory of the capture's own creation at
		// startup (CreateForScenes) stays fatal.
		[[nodiscard]] Result<Image> Capture(const ViewportScreenshotRequest& request, const RenderSnapshot& snapshot);

		// M18 (C18.1, R-2D-PIXEL; Docs/Decisions/0029-m18-contract.md decision 6): the output mapping of the last successful
		// Capture's rendered image (request.Width x request.Height, before MaxDimension's downscale), SceneRenderer's
		// GetOutputMapping: which of its pixels show which scene pixels, and which are letterbox. A caller that maps pixels of
		// a downscaled image scales them to the rendered size first. The identity at 1 x 1 before the first Capture.
		[[nodiscard]] RenderOutputMapping GetLastOutputMapping() const;
	private:
		// The capture's hooks in the recreation protocol (see the class comment). Errors (Recreate): those of
		// GraphicsDevice::CreateCommandList.
		void ReleaseGpuObjects();
		[[nodiscard]] Status RecreateGpuObjects();
		// M18 (C18.4, decision 43): the registration's TrimUnused hook: SceneRenderer::ReleaseTargets of the private
		// renderer, unless a Capture is running (see the class comment).
		void TrimUnused();
	private:
		GraphicsDevice* m_Device = nullptr; // documented back-reference
		Scope<SceneRenderer> m_SceneRenderer;
		Scope<Readback> m_Readback;
		nvrhi::CommandListHandle m_CommandList;
		uint64_t m_FrameIndex = 0;
		RenderOutputMapping m_LastOutputMapping{}; // GetLastOutputMapping
		// The registration with the device's recreation registry (CreateForScenes); declared last, so it is dropped first.
		GpuRecreationRegistration m_Recreation{};
	};

}
