#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiPlatformWindows.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/CommandListRecording.h"
#include "Engine/Graphics/GpuOutOfMemory.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Swapchain.h"
#include "Engine/ImGui/ImGuiRenderer.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Dear ImGui's platform windows, the renderer side (Docs/Decisions/0035-m24-contract.md decisions 18 and 19; the class
// comment). Dear ImGui calls the Renderer_* callbacks with the window set's context current, from ImGui::UpdatePlatformWindows
// (ImGuiLayer::EndFrame) and ImGui::DestroyPlatformWindows; they find the window set through io.BackendRendererUserData, the
// renderer side's own pointer (ImGuiRenderer keeps none), and keep each window's data in its viewport's RendererUserData. They
// only record what the window needs: a target is created by Render (or a recovery's Recreate), between recordings, where a
// creation that runs out of memory can be trimmed for and retried, and where a failure has an error to return.

namespace Engine {

	namespace Utils {

		// A viewport's size in framebuffer pixels: its size times its framebuffer scale, the scale its draw data carries (the
		// context's display scale until the platform side gives the viewport one).
		[[nodiscard]] static glm::uvec2 GetFramebufferSize(const ImGuiViewport& viewport, ImVec2 size)
		{
			const ImVec2 scale = viewport.FramebufferScale.x != 0.0f ? viewport.FramebufferScale : ImGui::GetIO().DisplayFramebufferScale;
			const auto pixels = [](float extent, float factor)
			{
				const float value = extent * factor;
				// The largest extent a 2D image has on every device the engine runs on; beyond it, nothing is drawn anyway.
				constexpr float MaxExtent = 16384.0f;
				return std::isfinite(value) && value > 0.0f ? static_cast<uint32_t>(std::min(std::round(value), MaxExtent)) : 0u;
			};
			return glm::uvec2(pixels(size.x, scale.x), pixels(size.y, scale.y));
		}

	}

	namespace {

		// One platform window's renderer data (its viewport's RendererUserData).
		struct PlatformWindow
		{
			ImGuiViewport* Viewport = nullptr;          // documented back-reference: the window's viewport outlives this data
			glm::uvec2 Size = glm::uvec2(0);            // its framebuffer size (Renderer_CreateWindow, Renderer_SetWindowSize)
			Scope<IImGuiPlatformWindowTarget> Target{}; // none before Render creates it, and after an out-of-memory creation
			// The size whose target creation ran out of memory after its trim and retry: not tried again until the size changes.
			std::optional<glm::uvec2> FailedSize{};
			nvrhi::CommandListHandle CommandList{}; // created by the window's first draw; released with the targets
		};

		// The default target: a swapchain on the platform window's GLFW window.
		class SwapchainPlatformWindowTarget final : public IImGuiPlatformWindowTarget
		{
		public:
			SwapchainPlatformWindowTarget(GraphicsDevice& device, void* nativeWindow, const SwapchainSpecification& specification, Scope<Swapchain> swapchain)
				: m_Device(&device), m_NativeWindow(nativeWindow), m_Specification(specification), m_Swapchain(std::move(swapchain))
			{
			}

			Result<nvrhi::IFramebuffer*> Acquire(uint32_t frameSlot) override
			{
				if (m_Swapchain == nullptr)
					return static_cast<nvrhi::IFramebuffer*>(nullptr);
				ENGINE_TRY_ASSIGN(const SwapchainAcquireStatus status, m_Swapchain->AcquireNextImage(frameSlot));
				if (status != SwapchainAcquireStatus::Acquired)
					return static_cast<nvrhi::IFramebuffer*>(nullptr);
				return m_Swapchain->GetCurrentFramebuffer();
			}

			void QueueFrameSemaphores() override { m_Swapchain->QueueFrameSemaphores(); }
			void Present() override { m_Swapchain->Present(); }
			// The swapchain compares the window's framebuffer size with its own at its next acquire, as the main window's does.
			void Resize(glm::uvec2 /*size*/) override {}
			// The swapchain and its surface go now, before the lost device's VkInstance (decision 19).
			void ReleaseGpuObjects() override { m_Swapchain.reset(); }

			Status RecreateGpuObjects() override
			{
				ENGINE_TRY_ASSIGN(m_Swapchain, Swapchain::CreateForNativeWindow(*m_Device, m_NativeWindow, m_Specification));
				return {};
			}
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference: the device outlives the window set's targets
			void* m_NativeWindow = nullptr;     // documented back-reference: the GLFW window outlives its target
			SwapchainSpecification m_Specification{};
			Scope<Swapchain> m_Swapchain{};
		};

	}

	Result<Scope<IImGuiPlatformWindowTarget>> CreateSwapchainPlatformWindowTarget(GraphicsDevice& device, const ImGuiPlatformWindowTargetRequest& request)
	{
		if (request.NativeWindow == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "a platform window's swapchain needs its native window, and the viewport has none");
		const SwapchainSpecification specification = { .VSync = request.VSync, .FramesInFlight = request.FramesInFlight };
		ENGINE_TRY_ASSIGN(Scope<Swapchain> swapchain,
			WithContext(Swapchain::CreateForNativeWindow(device, request.NativeWindow, specification), "while creating a platform window's swapchain"));
		return Scope<IImGuiPlatformWindowTarget>(CreateScope<SwapchainPlatformWindowTarget>(device, request.NativeWindow, specification, std::move(swapchain)));
	}

	struct ImGuiPlatformWindows::State
	{
		GraphicsDevice* Device = nullptr;  // documented back-reference
		ImGuiRenderer* Renderer = nullptr; // documented back-reference
		ImGuiContext* Context = nullptr;   // the context Create installed the callbacks on
		ImGuiPlatformWindowsSpecification Specification{};
		std::vector<Scope<PlatformWindow>> Windows{}; // in the order Dear ImGui created them

		// The window set whose callbacks the current context calls.
		[[nodiscard]] static State& GetCurrent()
		{
			State* state = static_cast<State*>(ImGui::GetIO().BackendRendererUserData);
			ENGINE_CORE_VERIFY(state != nullptr, "Dear ImGui called a platform window's renderer callback without a window set");
			return *state;
		}

		// Renderer_CreateWindow: the window's data, at its size; its target comes with Render.
		static void OnCreateWindow(ImGuiViewport* viewport)
		{
			State& state = GetCurrent();
			ENGINE_CORE_ASSERT(viewport->RendererUserData == nullptr, "Dear ImGui creates platform window {} twice", viewport->ID);
			state.Attach(*viewport);
		}

		// Renderer_DestroyWindow: the window's target and data. Dear ImGui calls it for the main viewport too, which has none.
		static void OnDestroyWindow(ImGuiViewport* viewport)
		{
			State& state = GetCurrent();
			if (viewport->RendererUserData == nullptr)
				return;
			const auto found = std::ranges::find_if(state.Windows, [viewport](const Scope<PlatformWindow>& window)
			{
				return window->Viewport == viewport;
			});
			ENGINE_CORE_ASSERT(found != state.Windows.end(), "platform window {} has renderer data the window set does not hold", viewport->ID);
			if (found != state.Windows.end())
				state.Windows.erase(found);
			viewport->RendererUserData = nullptr;
		}

		// Renderer_SetWindowSize: the window's new size, which its target takes at its next acquire; a window without a target
		// tries a creation again at a new size (decision 19).
		static void OnSetWindowSize(ImGuiViewport* viewport, ImVec2 size)
		{
			auto* window = static_cast<PlatformWindow*>(viewport->RendererUserData);
			if (window == nullptr)
				return;
			window->Size = Utils::GetFramebufferSize(*viewport, size);
			if (window->Target != nullptr)
				window->Target->Resize(window->Size);
		}

		void Attach(ImGuiViewport& viewport)
		{
			Scope<PlatformWindow> window = CreateScope<PlatformWindow>();
			window->Viewport = &viewport;
			window->Size = Utils::GetFramebufferSize(viewport, viewport.Size);
			viewport.RendererUserData = window.get();
			Windows.push_back(std::move(window));
		}

		[[nodiscard]] ImGuiPlatformWindowTargetRequest MakeRequest(const PlatformWindow& window) const
		{
			return {
				.NativeWindow = window.Viewport->PlatformHandle,
				.Size = window.Size,
				.FramesInFlight = Specification.FramesInFlight,
				.VSync = Specification.VSync,
			};
		}

		// The window's target, created through the factory: a target the specification's factory creates is a request after
		// startup (RetryAfterTrim, with every Gpu error on a healthy device out of memory, as the factory's errors are), while
		// the default target applies that rule itself with the swapchain's own classification (Swapchain::CreateForNativeWindow).
		// A creation that fails on a healthy device leaves the window without a target until its size changes, so a creation
		// that cannot succeed is not tried again every frame, each failed swapchain waiting for the device to go idle (M24's
		// review): one that ran out of memory after its trim and retry is logged at Warn and is no error. Errors: any other
		// failure of the creation (a swapchain's as Gpu, never out of memory; Unsupported or InvalidArgument for a window the
		// device cannot present to), once for each size it fails at.
		[[nodiscard]] Status CreateTarget(PlatformWindow& window)
		{
			const ImGuiPlatformWindowTargetRequest request = MakeRequest(window);
			Scope<IImGuiPlatformWindowTarget> target;
			const auto attempt = [this, &target, &request]() -> Status
			{
				if (Specification.CreateTarget)
				{
					ENGINE_TRY_ASSIGN(target, Specification.CreateTarget(*Device, request));
				}
				else
				{
					ENGINE_TRY_ASSIGN(target, CreateSwapchainPlatformWindowTarget(*Device, request));
				}
				return {};
			};
			const std::string description = std::format("a platform window's {}x{} target", window.Size.x, window.Size.y);
			const Status created = Specification.CreateTarget ? RetryAfterTrim(*Device, description, attempt) : attempt();
			if (created.has_value())
			{
				window.Target = std::move(target);
				window.FailedSize.reset();
				return {};
			}
			// On a lost device the failure is the loss: the recovery's Recreate creates the target again.
			if (Device->IsLost())
				return created;
			window.FailedSize = window.Size;
			if (created.error().GetCode() == ErrorCode::OutOfMemory)
			{
				ENGINE_CORE_WARN("The platform window of viewport {:08X} draws nothing until its size changes: {}", window.Viewport->ID, created.error());
				return {};
			}
			return created;
		}

		// A recording's failure to create what it records with (the window's command list, the renderer's pipeline for the
		// target's format, a geometry buffer, a binding set) is the device out of memory, the main UI's rule (decision 18,
		// Application's ImGuiLayer::Render): its Gpu error on a healthy device comes back as OutOfMemory, so the application
		// tells it from a target's (a swapchain's) failure, which stays Gpu, as the main window's swapchain's does (M24's
		// review).
		[[nodiscard]] Status AsRecordingError(Status status) const
		{
			if (status.has_value() || status.error().GetCode() != ErrorCode::Gpu || Device->IsLost())
				return status;
			return MakeError(ErrorCode::OutOfMemory, "cannot record a platform window, out of GPU memory: {}", status.error().ToString());
		}

		// Draws one platform window (Render).
		[[nodiscard]] Status RenderWindow(PlatformWindow& window, ImDrawData& drawData, uint32_t frameSlot)
		{
			if (window.Target == nullptr)
			{
				if (window.FailedSize == window.Size)
					return {};
				ENGINE_TRY(CreateTarget(window));
				if (window.Target == nullptr)
					return {};
			}
			// The command list before the acquire: an acquired image is always submitted and presented.
			if (window.CommandList == nullptr)
			{
				Result<nvrhi::CommandListHandle> commandList = Device->CreateCommandList();
				if (!commandList)
					return AsRecordingError(std::unexpected(std::move(commandList).error()));
				window.CommandList = std::move(*commandList);
			}

			Result<nvrhi::IFramebuffer*> acquired = window.Target->Acquire(frameSlot);
			if (!acquired.has_value())
			{
				// A recreation at the window's new size that ran out of memory after its trim and retry: the window goes without a
				// target, as after its creation, and the next size change creates one again.
				if (acquired.error().GetCode() != ErrorCode::OutOfMemory || Device->IsLost())
					return std::unexpected(std::move(acquired).error());
				ENGINE_CORE_WARN("The platform window of viewport {:08X} draws nothing until its size changes: {}", window.Viewport->ID, acquired.error());
				window.Target.reset();
				window.FailedSize = window.Size;
				return {};
			}
			if (*acquired == nullptr)
				return {};

			nvrhi::IFramebuffer& framebuffer = **acquired;
			nvrhi::ICommandList& commandList = *window.CommandList;
			CommandListRecording recording(*Device, commandList);
			// Every frame clears its window, as Application clears the main window, so a window draws the same pixels whatever
			// its swapchain image held.
			commandList.clearTextureFloat(framebuffer.getDesc().colorAttachments[0].texture, nvrhi::AllSubresources,
				nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]));
			// A failed recording draws nothing (ImGuiRenderer records completely or not at all); the image is still submitted,
			// with its semaphores, and presented, so the swapchain's acquire and present stay paired.
			const Status rendered = Renderer->RenderDrawData(commandList, framebuffer, drawData);
			window.Target->QueueFrameSemaphores();
			static_cast<void>(recording.Execute());
			window.Target->Present();
			return AsRecordingError(rendered);
		}

		// The recreation protocol's Release (decision 19): every target's GPU objects and every command list.
		void Release()
		{
			for (const Scope<PlatformWindow>& window : Windows)
			{
				if (window->Target != nullptr)
					window->Target->ReleaseGpuObjects();
				window->CommandList = nullptr;
			}
		}

		// The recreation protocol's Recreate: a target again for every platform window that still exists, at its current
		// size, also one that had none (created while the device was lost, or after an out-of-memory creation). Errors: the
		// first target's; an out-of-memory creation is a second startup's here, which fails the recovery.
		[[nodiscard]] Status Recreate()
		{
			for (const Scope<PlatformWindow>& window : Windows)
			{
				window->FailedSize.reset();
				if (window->Target != nullptr)
				{
					ENGINE_TRY(WithContext(window->Target->RecreateGpuObjects(), std::format("while recreating the platform window of viewport {:08X}", window->Viewport->ID)));
					continue;
				}
				const ImGuiPlatformWindowTargetRequest request = MakeRequest(*window);
				Result<Scope<IImGuiPlatformWindowTarget>> target =
					Specification.CreateTarget ? Specification.CreateTarget(*Device, request) : CreateSwapchainPlatformWindowTarget(*Device, request);
				if (!target.has_value())
					return std::unexpected(std::move(target).error().WithContext(std::format("while creating the platform window of viewport {:08X}", window->Viewport->ID)));
				window->Target = std::move(*target);
			}
			return {};
		}
	};

	ImGuiPlatformWindows::ImGuiPlatformWindows(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	ImGuiPlatformWindows::~ImGuiPlatformWindows()
	{
		// The registration first: no recovery may reach the window set while it goes.
		m_Recreation.Reset();
		if (m_State->Context == nullptr)
			return;

		const bool isContextCurrent = ImGui::GetCurrentContext() == m_State->Context;
		ENGINE_CORE_ASSERT(isContextCurrent, "ImGuiPlatformWindows destroyed while another Dear ImGui context (or none) is current");
		// The targets before the platform side's windows: a swapchain and its surface go before the GLFW window they are on
		// (ImGuiLayer destroys the window set before the GLFW backend's shutdown destroys the windows).
		m_State->Windows.clear();
		if (!isContextCurrent)
			return;
		ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
		platform.Renderer_CreateWindow = nullptr;
		platform.Renderer_DestroyWindow = nullptr;
		platform.Renderer_SetWindowSize = nullptr;
		for (ImGuiViewport* viewport : platform.Viewports)
			viewport->RendererUserData = nullptr;
		ImGuiIO& io = ImGui::GetIO();
		io.BackendFlags &= ~ImGuiBackendFlags_RendererHasViewports;
		io.BackendRendererUserData = nullptr;
	}

	Result<Scope<ImGuiPlatformWindows>> ImGuiPlatformWindows::Create(GraphicsDevice& device, ImGuiRenderer& renderer,
		const ImGuiPlatformWindowsSpecification& specification)
	{
		ImGuiContext* context = ImGui::GetCurrentContext();
		if (context == nullptr)
			return MakeError(ErrorCode::InvalidState, "the platform windows' renderer side needs a current Dear ImGui context");
		ImGuiIO& io = ImGui::GetIO();
		if ((io.BackendFlags & ImGuiBackendFlags_PlatformHasViewports) == 0)
			return MakeError(ErrorCode::InvalidState, "the Dear ImGui context's platform side has no viewports (ImGuiBackendFlags_PlatformHasViewports)");
		if (io.BackendRendererUserData != nullptr)
			return MakeError(ErrorCode::InvalidState, "the Dear ImGui context already has a renderer side for its platform windows");
		if (specification.FramesInFlight == 0)
			return MakeError(ErrorCode::InvalidArgument, "the platform windows need at least one frame in flight");

		Scope<ImGuiPlatformWindows> windows = CreateScope<ImGuiPlatformWindows>(ConstructionKey());
		State& state = *windows->m_State;
		state.Device = &device;
		state.Renderer = &renderer;
		state.Context = context;
		state.Specification = specification;

		io.BackendRendererUserData = &state;
		io.BackendFlags |= ImGuiBackendFlags_RendererHasViewports;
		ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
		platform.Renderer_CreateWindow = &State::OnCreateWindow;
		platform.Renderer_DestroyWindow = &State::OnDestroyWindow;
		platform.Renderer_SetWindowSize = &State::OnSetWindowSize;
		// A platform window that exists already (one created before this renderer side) gets its data now, as if Dear ImGui
		// had just created it.
		for (int index = 1; index < platform.Viewports.Size; ++index)
		{
			ImGuiViewport* viewport = platform.Viewports[index];
			if (viewport->PlatformHandle != nullptr && viewport->RendererUserData == nullptr)
				state.Attach(*viewport);
		}

		// Registered after the ImGuiRenderer it draws with (created before it), so a loss releases the window set first and
		// creates it again after the renderer (decision 19; ADR 0029 decision 26).
		State* owner = &state;
		GpuRecreationHooks hooks;
		hooks.Name = "ImGuiPlatformWindows";
		hooks.Release = [owner]()
		{
			owner->Release();
		};
		hooks.Recreate = [owner]()
		{
			return owner->Recreate();
		};
		windows->m_Recreation = device.GetRecreation().Register(std::move(hooks));
		return windows;
	}

	Status ImGuiPlatformWindows::Render(uint32_t frameSlot)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == state.Context, "ImGuiPlatformWindows::Render with another Dear ImGui context current");
		if (state.Device->IsLost())
			return {};

		Status first{};
		ImGuiPlatformIO& platform = ImGui::GetPlatformIO();
		// The main viewport (index 0) is the application's.
		for (int index = 1; index < platform.Viewports.Size; ++index)
		{
			ImGuiViewport* viewport = platform.Viewports[index];
			auto* window = static_cast<PlatformWindow*>(viewport->RendererUserData);
			if (window == nullptr || (viewport->Flags & ImGuiViewportFlags_IsMinimized) != 0 || viewport->DrawData == nullptr || !viewport->DrawData->Valid)
				continue;
			const Status drawn = state.RenderWindow(*window, *viewport->DrawData, frameSlot);
			if (!drawn.has_value() && first.has_value())
				first = drawn;
			// A loss during a window's frame ends the frame here: the frame boundary recovers the device.
			if (state.Device->IsLost())
				break;
		}
		return first;
	}

	size_t ImGuiPlatformWindows::GetWindowCount() const
	{
		return static_cast<size_t>(std::ranges::count_if(m_State->Windows, [](const Scope<PlatformWindow>& window)
		{
			return window->Target != nullptr;
		}));
	}

}
