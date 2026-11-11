#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/GpuRecreation.h"

#include <glm/vec2.hpp>
#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <functional>

// Dear ImGui's platform windows, the renderer side (M24, C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decisions 18
// and 19; stream V): with ImGuiConfigFlags_ViewportsEnable, a panel dragged out of the main window gets an OS window of its own
// (an ImGui viewport), which the vendored GLFW backend creates as a GLFW window (the platform side, its Platform_* callbacks)
// and this class draws (the Renderer_* callbacks of ImGuiPlatformIO, ImGuiBackendFlags_RendererHasViewports): one target per
// platform window, a swapchain on its window by default, into which the window's ImDrawData is recorded through the
// application's ImGuiRenderer (one pipeline per target format, as for the main window), each window on its own command list,
// submitted with its swapchain's semaphores and presented. Only ImGuiLayer creates one, and only where
// CheckPlatformWindowSupport (Platform/PlatformWindowSupport.h) allows platform windows: on Windows, in a windowed process.
//
// The recreation protocol (decision 19; ADR 0029 decisions 26 and 27): Create registers the platform windows as one owner,
// "ImGuiPlatformWindows", after the ImGuiRenderer it draws with, so a device loss releases them first and creates them again
// after it. Release drops every window's target (a swapchain releases its swapchain and surface before the old VkInstance is
// destroyed) and command list, on a lost device and possibly in the middle of a frame the loss ended; Recreate creates a target
// again for every platform window that still exists, at its current size, on the recreated device (a new surface on the new
// instance). A window created while the device is lost gets its target at the recovery.
//
// Out of GPU memory (decision 19; M18's rule after startup, ADR 0029 decisions 42 and 44): a platform window's target is
// created after startup (when a panel is undocked) and again at its resize, so each creation is a request like the frame's
// resize: an out-of-memory creation is trimmed and retried once (RetryAfterTrim; the default target with the swapchain's own
// out-of-memory classification, as Swapchain::AcquireNextImage's recreation passes it), and a second one leaves that window
// without a target, logged at Warn and never fatal: it draws nothing, the other windows and the frame go on, and its
// creation is tried again at its next size change. A target creation that fails otherwise is Render's Gpu error. In a
// recovery's Recreate an out-of-memory creation is a second startup's, which fails the recovery, as every owner's.
//
// Synchronization: each target owns its acquire and present semaphores (Swapchain's), so a window's frame waits only for its own
// image, and FramePacer covers the windows' submissions with the frame's (ImGuiLayer::RenderPlatformWindows runs before the
// frame's FramePacer::EndFrame); clean under synchronization validation.

namespace Engine {

	class GraphicsDevice;
	class ImGuiRenderer;

	// What one platform window's target is created for.
	struct ImGuiPlatformWindowTargetRequest
	{
		void* NativeWindow = nullptr;    // the GLFWwindow* of the viewport (ImGuiViewport::PlatformHandle); null for a test's targets
		glm::uvec2 Size = glm::uvec2(0); // the viewport's framebuffer size in pixels (0 x 0 while minimized)
		uint32_t FramesInFlight = 2;     // GraphicsSpecification::FramesInFlight
		bool VSync = true;               // GraphicsSpecification::VSync
	};

	// One platform window's target: a swapchain on its native window (CreateSwapchainPlatformWindowTarget), or in tests an
	// offscreen target. Main thread. Its GPU objects are released and created again only through its window set's
	// registration, never a registration of its own.
	class IImGuiPlatformWindowTarget
	{
	public:
		virtual ~IImGuiPlatformWindowTarget() = default;

		// The framebuffer to record this frame's draw data into, or nullptr when the window shows nothing this frame (minimized,
		// its swapchain just recreated, an acquire that timed out, a lost device). Errors: Gpu or OutOfMemory as the swapchain's
		// AcquireNextImage.
		[[nodiscard]] virtual Result<nvrhi::IFramebuffer*> Acquire(uint32_t frameSlot) = 0;
		// Right before the window's submission, after an Acquire that gave a framebuffer.
		virtual void QueueFrameSemaphores() = 0;
		// After the window's submission.
		virtual void Present() = 0;
		// The window's framebuffer size changed (Renderer_SetWindowSize); the next Acquire recreates at it.
		virtual void Resize(glm::uvec2 size) = 0;
		// The recreation protocol's hooks, called by the window set's Release and Recreate (the class comment).
		virtual void ReleaseGpuObjects() = 0;
		[[nodiscard]] virtual Status RecreateGpuObjects() = 0;
	};

	// Creates a platform window's target. Errors: those of the target's creation (an out-of-memory one as a Gpu error, which the
	// window set retries after a trim: the class comment).
	using ImGuiPlatformWindowTargetFactory =
		std::function<Result<Scope<IImGuiPlatformWindowTarget>>(GraphicsDevice& device, const ImGuiPlatformWindowTargetRequest& request)>;

	// The default target: a swapchain on the request's native window (Swapchain::CreateForNativeWindow). Errors: those of
	// Swapchain::CreateForNativeWindow; InvalidArgument for a null NativeWindow.
	[[nodiscard]] Result<Scope<IImGuiPlatformWindowTarget>> CreateSwapchainPlatformWindowTarget(GraphicsDevice& device,
		const ImGuiPlatformWindowTargetRequest& request);

	struct ImGuiPlatformWindowsSpecification
	{
		uint32_t FramesInFlight = 2;                     // GraphicsSpecification::FramesInFlight, for the targets and command lists
		bool VSync = true;                               // GraphicsSpecification::VSync
		ImGuiPlatformWindowTargetFactory CreateTarget{}; // empty: CreateSwapchainPlatformWindowTarget
	};

	// The renderer side of the current Dear ImGui context's platform windows. Not copyable or movable; main thread only, with
	// the context current that it was created for.
	class ImGuiPlatformWindows
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ImGuiPlatformWindows;
		};

		explicit ImGuiPlatformWindows(ConstructionKey key);
		// Destroys every target (the device is idle: Application waits before) and uninstalls the Renderer_* callbacks.
		~ImGuiPlatformWindows();

		ImGuiPlatformWindows(const ImGuiPlatformWindows&) = delete;
		ImGuiPlatformWindows& operator=(const ImGuiPlatformWindows&) = delete;

		// Installs the Renderer_* callbacks and ImGuiBackendFlags_RendererHasViewports on the current context, and registers
		// "ImGuiPlatformWindows" with `device`'s recreation registry. `device` and `renderer` are documented back-references that
		// must outlive the window set. Errors: InvalidState without a current Dear ImGui context, or when its platform side has
		// no viewports (ImGuiBackendFlags_PlatformHasViewports); InvalidArgument for FramesInFlight 0.
		[[nodiscard]] static Result<Scope<ImGuiPlatformWindows>> Create(GraphicsDevice& device, ImGuiRenderer& renderer,
			const ImGuiPlatformWindowsSpecification& specification);

		// Draws every platform window (every viewport but the main one that has a target) whose target acquires a framebuffer:
		// records its draw data with the renderer on its own command list, submits it with its target's semaphores and presents
		// it. Call after ImGui::UpdatePlatformWindows (ImGuiLayer::EndFrame) and the main window's submission, before the
		// frame's FramePacer::EndFrame. Does nothing on a lost device. A target's out-of-memory creation or recreation (an
		// Acquire's OutOfMemory included) is never an error here: that window draws nothing (the class comment). Errors: the
		// first Gpu error of a window's recording, submission or target, and an out-of-memory of its recording (the main UI's
		// rule, decision 18); the other windows are still drawn.
		[[nodiscard]] Status Render(uint32_t frameSlot);

		// The platform windows that have a target now (a window whose target creation ran out of memory has none).
		[[nodiscard]] size_t GetWindowCount() const;
	private:
		struct State;
		Scope<State> m_State{};
		// The registration with the device's recreation registry; declared last, so it is dropped first.
		GpuRecreationRegistration m_Recreation{};
	};

}
