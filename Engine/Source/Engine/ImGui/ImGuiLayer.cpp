#include "EnginePCH.h"
#include "Engine/ImGui/ImGuiLayer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiPlatformWindows.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Engine/Platform/GlfwLibrary.h"
#include "Engine/Platform/PlatformWindowSupport.h"
#include "Engine/Platform/Window.h"

#include <GLFW/glfw3.h>
#include <backends/imgui_impl_glfw.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

// The platform side (Architecture §8.11). On a native platform (Win32, Cocoa, X11) it is the vendored GLFW backend,
// ImGui_ImplGlfw_InitForVulkan with its callbacks installed, compiled in ImGui/ImGuiGlfwImplementation.cpp.
//
// On GLFW's null platform (every headless process: the editor's screenshots and golden images, §8.13, and the GPU tests)
// the vendored backend cannot initialize: on Windows it subclasses the window procedure of the window's HWND and asserts
// that one exists (IM_ASSERT(bd->PrevWndProc != nullptr) after glfwGetWin32Window), and on macOS it asks for the Cocoa
// window, which GLFW reports as an error; a null-platform window has neither. The vendored source is never edited
// (AGENTS.md), so on the null platform the layer provides what the backend would: the display size and framebuffer scale
// from the window every frame, the frame clock's delta (as on native platforms), and the clipboard through GLFW, which
// keeps it in memory there. The null platform delivers no input (headless input reaches the application through
// automation and replay, never through the window, Window.h), so there is nothing else to forward, and every platform
// renders the same UI for the same frames.

namespace Engine {

	// io.BackendPlatformName on the null platform (Dear ImGui keeps the pointer).
	static constexpr const char* NullPlatformName = "Engine GLFW null platform";

	namespace Utils {

		// Creates the folder imgui.ini goes in; Dear ImGui's own writes fail silently without it.
		static Status CreateIniDirectory(const std::filesystem::path& iniFilePath)
		{
			const std::filesystem::path directory = iniFilePath.parent_path();
			if (directory.empty())
				return {};
			const Status created = FileSystem::CreateDirectories(directory);
			if (!created.has_value())
			{
				return MakeError(ErrorCode::Io, "cannot create the folder of the ImGui settings file '{}': {}", FileSystem::PathToUtf8(iniFilePath),
					created.error().ToString());
			}
			return {};
		}

		// The clipboard on the null platform: GLFW's, which the null platform keeps in process memory, so a headless run
		// never touches the user's clipboard (Dear ImGui's default on Windows is the system clipboard).
		static const char* GetNullPlatformClipboardText(ImGuiContext* /*context*/)
		{
			return glfwGetClipboardString(nullptr);
		}

		static void SetNullPlatformClipboardText(ImGuiContext* /*context*/, const char* text)
		{
			glfwSetClipboardString(nullptr, text);
		}

	}

	ImGuiLayer::ImGuiLayer(ConstructionKey /*key*/)
	{
	}

	ImGuiLayer::~ImGuiLayer()
	{
		if (m_Context == nullptr)
			return;

		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer destroyed while another Dear ImGui context is current");
		// The platform windows' renderer side first (M24): their swapchains and surfaces go before the GLFW windows the
		// backend's shutdown destroys, and before the renderer they draw with. Then the renderer: its destructor destroys the
		// textures (the device is idle; Application waits before) and clears its backend fields, then the platform side, then
		// the context, which saves imgui.ini when an ini path is set.
		m_PlatformWindows.reset();
		m_Renderer.reset();
		if (m_UsesGlfwBackend)
			ImGui_ImplGlfw_Shutdown();
		if (m_UsesNullPlatform)
			UninstallNullPlatform();
		ImGui::DestroyContext(m_Context);
	}

	Result<Scope<ImGuiLayer>> ImGuiLayer::Create(Window& window, GraphicsDevice& device, PipelineFactory& pipelines,
		const ImGuiLayerSpecification& specification)
	{
		// Dear ImGui's current context is process-wide; the layer owns the only one.
		if (ImGui::GetCurrentContext() != nullptr)
			return MakeError(ErrorCode::InvalidState, "a Dear ImGui context already exists; there is one ImGuiLayer per process at a time");
		if (!specification.IniFilePath.empty())
			ENGINE_TRY(Utils::CreateIniDirectory(specification.IniFilePath));

		// From here on the layer's destructor undoes whatever was set up, so every failure below leaves nothing behind.
		Scope<ImGuiLayer> layer = CreateScope<ImGuiLayer>(ConstructionKey());
		layer->m_Window = &window;
		layer->m_IniFilePath = specification.IniFilePath;
		layer->m_IniFileName = FileSystem::PathToUtf8(specification.IniFilePath);

		IMGUI_CHECKVERSION();
		layer->m_Context = ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		io.ConfigFlags &= ~ImGuiConfigFlags_ViewportsEnable;
		io.IniFilename = layer->m_IniFileName.empty() ? nullptr : layer->m_IniFileName.c_str();

		if (GlfwLibrary::GetPlatform() == GlfwPlatform::Null)
		{
			layer->InstallNullPlatform();
		}
		else
		{
			if (!ImGui_ImplGlfw_InitForVulkan(static_cast<GLFWwindow*>(window.GetNativeHandle()), true))
				return MakeError(ErrorCode::Unsupported, "Dear ImGui's GLFW backend cannot initialize on the window '{}'", window.GetTitle());
			layer->m_UsesGlfwBackend = true;
		}

		ENGINE_TRY_ASSIGN(layer->m_Renderer, ImGuiRenderer::Create(device, pipelines, { .FramesInFlight = specification.FramesInFlight }));
		layer->m_PlatformWindowSupport = layer->CreatePlatformWindows(device, specification);
		if (specification.PlatformWindows && layer->m_PlatformWindowSupport.has_value())
			ENGINE_CORE_INFO("Dear ImGui platform windows are on: panels may be undocked into windows of their own");
		else if (specification.PlatformWindows)
			ENGINE_CORE_INFO("Dear ImGui platform windows stay off, so every panel stays docked: {}", layer->m_PlatformWindowSupport.error());
		return layer;
	}

	void ImGuiLayer::BeginFrame(double deltaSeconds, uint32_t frameSlot, bool retainDisplaySize)
	{
		ENGINE_CORE_ASSERT(deltaSeconds >= 0.0 && std::isfinite(deltaSeconds), "ImGuiLayer::BeginFrame with the delta {}", deltaSeconds);
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::BeginFrame with another Dear ImGui context current");
		m_Renderer->BeginFrame(frameSlot);
		ImGuiIO& io = ImGui::GetIO();
		const ImVec2 previousSize = io.DisplaySize;
		const ImVec2 previousScale = io.DisplayFramebufferScale;
		if (m_UsesGlfwBackend)
			ImGui_ImplGlfw_NewFrame();
		else
			UpdateNullPlatform();
		if (retainDisplaySize)
		{
			io.DisplaySize = previousSize.x > 0.0f && previousSize.y > 0.0f
				? previousSize
				: ImVec2(static_cast<float>(std::max(m_Window->GetWidth(), 1u)), static_cast<float>(std::max(m_Window->GetHeight(), 1u)));
			io.DisplayFramebufferScale = previousScale;
		}
		m_RetainedDisplaySize = retainDisplaySize;
		// After the backend, which sets its own wall-clock delta (see the declaration).
		ImGui::GetIO().DeltaTime = static_cast<float>(std::max(deltaSeconds, MinimumDeltaSeconds));
		ImGui::NewFrame();
	}

	void ImGuiLayer::EndFrame()
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::EndFrame with another Dear ImGui context current");
		ImGui::Render();
		if (m_PlatformWindows != nullptr && m_RetainedDisplaySize)
		{
			// M24's review: with platform windows on, Dear ImGui asks the platform side whether the main viewport is minimized
			// and then gives its draw data an empty display (0 x 0), which BeginFrame's retained io.DisplaySize does not reach.
			// A frame forced while the window is minimized (Application::RequestOffscreenUiFrame) renders the main window's UI
			// at the size it was laid out at, the main viewport's, which Dear ImGui keeps while the window is minimized, so
			// editor.screenshot captures it as it does without platform windows.
			const ImGuiViewport* main = ImGui::GetMainViewport();
			if (ImDrawData* drawData = ImGui::GetDrawData(); drawData != nullptr && main != nullptr)
				drawData->DisplaySize = main->Size;
		}
		if (m_PlatformWindows != nullptr)
		{
			// The GLFW backend creates a platform window with GLFW's current window hints, and a window the swapchain's Vulkan
			// surface goes on must have no client API (Window::Create sets the same hint for the main window).
			glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
			ImGui::UpdatePlatformWindows();
		}
	}

	void ImGuiLayer::EndInterruptedFrame()
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::EndInterruptedFrame with another Dear ImGui context current");
		if (!m_Context->WithinFrameScope)
			return;
		// Dear ImGui's recovery after an exception (imgui.h, "Recovery after error/exception"): with assertions, the log
		// and the tooltip off, EndFrame closes what the frame left open back to the state NewFrame recorded. The user's
		// settings return afterwards.
		ImGuiIO& io = ImGui::GetIO();
		const bool recovery = io.ConfigErrorRecovery;
		const bool asserts = io.ConfigErrorRecoveryEnableAssert;
		const bool debugLog = io.ConfigErrorRecoveryEnableDebugLog;
		const bool tooltip = io.ConfigErrorRecoveryEnableTooltip;
		io.ConfigErrorRecovery = true;
		io.ConfigErrorRecoveryEnableAssert = false;
		io.ConfigErrorRecoveryEnableDebugLog = false;
		io.ConfigErrorRecoveryEnableTooltip = false;
		ImGui::EndFrame();
		// M24: with platform windows on, Dear ImGui expects every frame to end with ImGui::UpdatePlatformWindows. The
		// interrupted frame leaves the platform windows as they are (their targets are the recreation protocol's, released and
		// created again by the recovery), so it is only marked as ended for them.
		if (m_PlatformWindows != nullptr)
			m_Context->FrameCountPlatformEnded = m_Context->FrameCount;
		io.ConfigErrorRecovery = recovery;
		io.ConfigErrorRecoveryEnableAssert = asserts;
		io.ConfigErrorRecoveryEnableDebugLog = debugLog;
		io.ConfigErrorRecoveryEnableTooltip = tooltip;
	}

	Status ImGuiLayer::Render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer)
	{
		ImDrawData* drawData = GetDrawData();
		if (drawData == nullptr)
			return MakeError(ErrorCode::InvalidState, "ImGuiLayer::Render needs the draw data of a finished UI frame (EndFrame)");
		return m_Renderer->RenderDrawData(commandList, framebuffer, *drawData);
	}

	Status ImGuiLayer::SetIniFilePath(const std::filesystem::path& iniFilePath)
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::SetIniFilePath with another Dear ImGui context current");
		// The new folder first: when it cannot be created, the current file stays in effect.
		if (!iniFilePath.empty())
			ENGINE_TRY(Utils::CreateIniDirectory(iniFilePath));

		// Dear ImGui loads io.IniFilename at the first NewFrame. Before that there is nothing to save (saving would replace
		// the current file with empty settings) and nothing to reload: the first frame loads the new file.
		const bool areSettingsLoaded = m_Context->SettingsLoaded;
		if (areSettingsLoaded && !m_IniFileName.empty())
			ImGui::SaveIniSettingsToDisk(m_IniFileName.c_str());

		m_IniFilePath = iniFilePath;
		m_IniFileName = FileSystem::PathToUtf8(iniFilePath);
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = m_IniFileName.empty() ? nullptr : m_IniFileName.c_str();

		// The new file replaces the settings rather than merging into them (§8.11: the project's layout, not the
		// launcher's). A file that does not exist yet leaves the settings empty, so the live windows keep their places.
		if (areSettingsLoaded)
		{
			ImGui::ClearIniSettings();
			if (io.IniFilename != nullptr)
				ImGui::LoadIniSettingsFromDisk(io.IniFilename);
		}
		return {};
	}

	const std::filesystem::path& ImGuiLayer::GetIniFilePath() const
	{
		return m_IniFilePath;
	}

	ImGuiRenderer& ImGuiLayer::GetRenderer()
	{
		ENGINE_CORE_VERIFY(m_Renderer != nullptr, "ImGuiLayer::GetRenderer before the layer was created");
		return *m_Renderer;
	}

	ImDrawData* ImGuiLayer::GetDrawData() const
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::GetDrawData with another Dear ImGui context current");
		// Dear ImGui's draw data is valid from Render (EndFrame) to the next NewFrame; before the first EndFrame it is null.
		return ImGui::GetDrawData();
	}

	Status ImGuiLayer::GetPlatformWindowSupport() const
	{
		return m_PlatformWindowSupport;
	}

	Status ImGuiLayer::RenderPlatformWindows(uint32_t frameSlot)
	{
		ENGINE_CORE_ASSERT(ImGui::GetCurrentContext() == m_Context, "ImGuiLayer::RenderPlatformWindows with another Dear ImGui context current");
		if (m_PlatformWindows == nullptr)
			return {};
		return m_PlatformWindows->Render(frameSlot);
	}

	ImGuiPlatformWindows* ImGuiLayer::GetPlatformWindows()
	{
		return m_PlatformWindows.get();
	}

	Status ImGuiLayer::CreatePlatformWindows(GraphicsDevice& device, const ImGuiLayerSpecification& specification)
	{
		if (!specification.PlatformWindows)
			return MakeError(ErrorCode::InvalidState, "platform windows are off: the application did not ask for them");
		// Windows backends only (decision 17): Linux and macOS, and a headless process, refuse them here.
		ENGINE_TRY(CheckPlatformWindowSupport());
		if (!m_UsesGlfwBackend)
			return MakeError(ErrorCode::Unsupported, "platform windows need Dear ImGui's GLFW backend, which this process does not use");
		ENGINE_TRY_ASSIGN(m_PlatformWindows, WithContext(ImGuiPlatformWindows::Create(device, *m_Renderer, {
																											   .FramesInFlight = specification.FramesInFlight,
																											   .VSync = device.GetSpecification().VSync,
																											   .CreateTarget = {},
																										   }),
												 "while creating the platform windows' renderer side"));
		// Before the first frame, as Dear ImGui requires: from then on panels may be undocked into OS windows of their own.
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
		return {};
	}

	void ImGuiLayer::InstallNullPlatform()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.BackendPlatformName = NullPlatformName;
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Platform_GetClipboardTextFn = &Utils::GetNullPlatformClipboardText;
		platformIO.Platform_SetClipboardTextFn = &Utils::SetNullPlatformClipboardText;
		m_UsesNullPlatform = true;
	}

	void ImGuiLayer::UninstallNullPlatform()
	{
		ImGuiIO& io = ImGui::GetIO();
		io.BackendPlatformName = nullptr;
		ImGuiPlatformIO& platformIO = ImGui::GetPlatformIO();
		platformIO.Platform_GetClipboardTextFn = nullptr;
		platformIO.Platform_SetClipboardTextFn = nullptr;
		m_UsesNullPlatform = false;
	}

	void ImGuiLayer::UpdateNullPlatform()
	{
		const uint32_t width = m_Window->GetWidth();
		const uint32_t height = m_Window->GetHeight();
		const uint32_t framebufferWidth = m_Window->GetFramebufferWidth();
		const uint32_t framebufferHeight = m_Window->GetFramebufferHeight();
		ImGuiIO& io = ImGui::GetIO();
		io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
		io.DisplayFramebufferScale = ImVec2(width > 0 ? static_cast<float>(framebufferWidth) / static_cast<float>(width) : 1.0f,
			height > 0 ? static_cast<float>(framebufferHeight) / static_cast<float>(height) : 1.0f);
	}

}
