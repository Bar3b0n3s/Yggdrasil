#include "EnginePCH.h"
#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"

#include <format>
#include <limits>
#include <utility>

namespace Engine {

	namespace Utils {

		// The capture's command list. Not an immediate command list: NVRHI's validation allows one open immediate list at a
		// time, and a capture may be requested while the caller's frame has its own list open.
		static Result<nvrhi::CommandListHandle> CreateCaptureCommandList(GraphicsDevice& device)
		{
			return device.CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false));
		}

		// Sets ViewportCapture's capturing flag for its lifetime, so the flag drops however a capture ends (a vk::SystemError
		// thrown out of NVRHI unwinds through it too).
		class CapturingScope
		{
		public:
			explicit CapturingScope(bool& capturing)
				: m_Capturing(&capturing)
			{
				*m_Capturing = true;
			}

			~CapturingScope()
			{
				*m_Capturing = false;
			}

			CapturingScope(const CapturingScope&) = delete;
			CapturingScope& operator=(const CapturingScope&) = delete;
		private:
			bool* m_Capturing = nullptr; // the capture's m_IsCapturing, which outlives the scope
		};

	}

	ViewportCapture::ViewportCapture(ConstructionKey /*key*/)
	{
	}

	ViewportCapture::~ViewportCapture() = default;

	Result<Scope<ViewportCapture>> ViewportCapture::CreateForScenes(GraphicsDevice& device, SceneRendererPipelines& pipelines,
		GpuResourceCache& cache, AssetManager& assets)
	{
		ENGINE_TRY_ASSIGN(Scope<SceneRenderer> renderer, SceneRenderer::Create(device, pipelines, cache, assets, { .Width = 1, .Height = 1 }));
		ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle commandList, Utils::CreateCaptureCommandList(device));

		Scope<ViewportCapture> capture = CreateScope<ViewportCapture>(ConstructionKey());
		capture->m_Device = &device;
		capture->m_SceneRenderer = std::move(renderer);
		capture->m_Readback = CreateScope<Readback>(device);
		capture->m_CommandList = std::move(commandList);

		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 33): registered after its private renderer (which
		// registered itself in SceneRenderer::Create), so it is released before the renderer and created again after it.
		ViewportCapture* owner = capture.get();
		GpuRecreationHooks hooks;
		hooks.Name = "ViewportCapture";
		hooks.Release = [owner]()
		{
			owner->ReleaseGpuObjects();
		};
		hooks.Recreate = [owner]()
		{
			return owner->RecreateGpuObjects();
		};
		// M18 (C18.4, decision 43): the last capture's targets, when another request ran out of GPU memory.
		hooks.TrimUnused = [owner]()
		{
			owner->TrimUnused();
		};
		capture->m_Recreation = device.GetRecreation().Register(std::move(hooks));
		return capture;
	}

	void ViewportCapture::ReleaseGpuObjects()
	{
		// The read-back command list and the Readback's: a new Readback holds no GPU object until its first ReadTexture.
		m_CommandList = nullptr;
		m_Readback = CreateScope<Readback>(*m_Device);
	}

	Status ViewportCapture::RecreateGpuObjects()
	{
		ENGINE_TRY_ASSIGN(m_CommandList, Utils::CreateCaptureCommandList(*m_Device));
		return {};
	}

	void ViewportCapture::TrimUnused()
	{
		// Inside a capture, its own requests' trims leave the targets to it (the class comment); between captures nothing
		// displays the last capture's image, so its targets go, and its renderer's own hook then trims the pool.
		if (m_IsCapturing)
			return;
		m_SceneRenderer->ReleaseTargets();
	}

	Result<Image> ViewportCapture::Capture(const ViewportScreenshotRequest& request, const RenderSnapshot& snapshot)
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

		// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 to 45): the capture is one request. Its attempt is
		// submitted (and its read-back waited for) before it returns, so the trim of its retry runs between recordings; the
		// renderer's own requests (Resize, a debug view's first pipelines) trim and retry inside it, and an OutOfMemory they
		// return is not trimmed for again. A failed capture keeps GetLastOutputMapping.
		const Utils::CapturingScope capturing(m_IsCapturing);
		Image image;
		const Status captured = RetryAfterTrim(*m_Device, std::format("a {}x{} viewport capture", request.Width, request.Height),
			[this, &request, &snapshot, &image]() -> Status
		{
			// A capture at another size first releases the previous size's targets, so it does not hold them while it creates
			// the new ones (decision 43).
			if (m_SceneRenderer->GetWidth() != request.Width || m_SceneRenderer->GetHeight() != request.Height)
				m_SceneRenderer->ReleaseTargets();
			ENGINE_TRY(m_SceneRenderer->Resize(request.Width, request.Height));
			ENGINE_CORE_VERIFY(m_FrameIndex < std::numeric_limits<uint64_t>::max(), "capture frame identity exhausted");
			RenderSnapshot captured = snapshot;
			captured.FrameIndex = ++m_FrameIndex;
			// The list is executed whatever Render reports (an error names a skipped draw; the rest rendered).
			CommandListRecording recording(*m_Device, *m_CommandList);
			const Status rendered = m_SceneRenderer->Render(*m_CommandList, captured);
			const uint64_t submission = recording.Execute();
			m_SceneRenderer->OnSubmitted(captured.FrameIndex, submission);
			// A debug view that stayed on another mode is OutOfMemory: never an image of another debug view (decision 44).
			ENGINE_TRY(rendered);

			// The view-resolution image: the upscaled scene image in its letterbox when the snapshot's output stage scales it.
			ENGINE_TRY_ASSIGN(Image read, m_Readback->ReadTexture(*m_SceneRenderer->GetFinalTexture()));
			if (request.MaxDimension > 0)
			{
				ENGINE_TRY_ASSIGN(read, DownscaleImage(read, request.MaxDimension));
			}
			m_LastOutputMapping = m_SceneRenderer->GetOutputMapping();
			image = std::move(read);
			return {};
		});
		ENGINE_TRY(captured);
		return image;
	}

	RenderOutputMapping ViewportCapture::GetLastOutputMapping() const
	{
		return m_LastOutputMapping;
	}

}
