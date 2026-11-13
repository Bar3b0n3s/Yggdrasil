#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>
#include <string>

// Dear ImGui for an application (Architecture §8.11): the context, the vendored GLFW platform backend
// (ImGui_ImplGlfw_InitForVulkan, compiled in ImGui/ImGuiGlfwImplementation.cpp), the NVRHI renderer (ImGuiRenderer) and
// the imgui.ini location. Docking is on; multi-viewport (platform windows) only where the specification asks for it and
// CheckPlatformWindowSupport allows it (M24; ImGuiLayerSpecification::PlatformWindows). Headless processes run on GLFW's
// null platform, where the vendored backend cannot initialize (on Windows it asserts that the window has an HWND to
// subclass), so there the layer is the platform backend itself: the display size and framebuffer scale from the window
// each frame and GLFW's in-memory clipboard (ImGui/ImGuiLayer.cpp). The full editor UI therefore renders offscreen there
// (editor.screenshot, §8.13, §13.9).

namespace Engine {

	class GraphicsDevice;
	class ImGuiPlatformWindows;
	class ImGuiRenderer;
	class PipelineFactory;
	class Window;

	struct ImGuiLayerSpecification
	{
		// The native path of imgui.ini (io.IniFilename). Before a project opens it is <UserData>/<AppName>/Editor/imgui.ini,
		// the native location of user://Editor/imgui.ini (§8.11), and once one is open <Project>/Library/Editor/imgui.ini
		// (SetIniFilePath). Empty: no ini file (io.IniFilename = nullptr). Create creates the parent directory.
		std::filesystem::path IniFilePath{};
		// GraphicsSpecification::FramesInFlight, for the renderer.
		uint32_t FramesInFlight = 2;
		// M24 (C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decisions 17 and 18; stream V): Dear ImGui's platform
		// windows, panels undocked into OS windows of their own. Create sets ImGuiConfigFlags_ViewportsEnable and creates the
		// ImGuiPlatformWindows (ImGui/ImGuiPlatformWindows.h) only when this is set, the GLFW backend is in use and
		// CheckPlatformWindowSupport (Platform/PlatformWindowSupport.h) allows them; otherwise the flag stays cleared, as before
		// M24, and GetPlatformWindowSupport says why. The editor sets it from the user's PlatformWindows preference
		// (ApplicationSpecification::ImGuiPlatformWindows); the Runtime's overlay never does.
		bool PlatformWindows = false;
	};

	// One per process at a time (Dear ImGui's current context is global; Create fails while a context exists). Not copyable
	// or movable; main thread only.
	class ImGuiLayer
	{
	public:
		// The smallest io.DeltaTime BeginFrame sets (a microsecond): Dear ImGui requires DeltaTime > 0.
		static constexpr double MinimumDeltaSeconds = 1.0e-6;

		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class ImGuiLayer;
		};

		// Use Create.
		explicit ImGuiLayer(ConstructionKey key);
		// DestroyTextures on the renderer (the device must be idle: Application waits before), ImGui_ImplGlfw_Shutdown on a
		// native platform, then ImGui::DestroyContext, which saves imgui.ini when an ini path is set.
		~ImGuiLayer();

		ImGuiLayer(const ImGuiLayer&) = delete;
		ImGuiLayer& operator=(const ImGuiLayer&) = delete;

		// Creates the context (docking enabled, viewports disabled, the ini path), initializes the GLFW backend on `window`
		// with its callbacks installed (they chain to the Window's own) on a native platform, or the layer's own platform
		// side on GLFW's null platform (see the file comment), and creates the ImGuiRenderer. M24: with
		// ImGuiLayerSpecification::PlatformWindows, then the ImGuiPlatformWindows and ImGuiConfigFlags_ViewportsEnable where
		// CheckPlatformWindowSupport allows them on the GLFW backend; a refusal or a failure of that step only leaves them off
		// (GetPlatformWindowSupport says why, and one Info line). `window`, `device` and `pipelines` are documented
		// back-references that must outlive the layer. Errors: InvalidState when another ImGuiLayer (any Dear ImGui context)
		// exists; Io when the ini directory cannot be created; Unsupported when the GLFW backend cannot initialize; those of
		// ImGuiRenderer::Create. Nothing stays created on failure.
		[[nodiscard]] static Result<Scope<ImGuiLayer>> Create(Window& window, GraphicsDevice& device, PipelineFactory& pipelines,
			const ImGuiLayerSpecification& specification);

		// Starts a UI frame: ImGuiRenderer::BeginFrame(frameSlot), ImGui_ImplGlfw_NewFrame (on the null platform the
		// window's display size and framebuffer scale), then io.DeltaTime =
		// max(`deltaSeconds`, MinimumDeltaSeconds), then ImGui::NewFrame. Widgets may be submitted until EndFrame.
		// `deltaSeconds` (>= 0, finite; asserted) is the frame clock's time since the previous UI frame, which Application
		// accumulates over the frames the swapchain skipped. It replaces the backend's wall-clock delta, so headless runs on a
		// ManualClock render identical frames (the ImGuiDemo golden). Dear ImGui asserts a positive DeltaTime, and a
		// SystemClock's first delta is 0, hence the minimum.
		// A forced minimized frame retains the last usable display size and scale after updating backend input.
		void BeginFrame(double deltaSeconds, uint32_t frameSlot, bool retainDisplaySize = false);

		// ImGui::Render: finishes the frame's draw data.
		void EndFrame();

		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 26 and risk 10): ends a UI frame that BeginFrame started
		// and that never reached EndFrame, because a device loss ended the application's frame inside it (the frame-boundary
		// catch). The windows and stacks the interrupted frame left open are closed with Dear ImGui's error recovery, without
		// its assertions, debug log or error tooltip, and the frame ends without draw data (GetDrawData is null until the
		// next EndFrame), so the next BeginFrame starts a new frame. CPU only: the application frame's release calls it on a
		// lost device. Does nothing when no UI frame is open.
		void EndInterruptedFrame();

		// Records the draw data of the last EndFrame into `commandList` targeting `framebuffer` (ImGuiRenderer::RenderDrawData).
		// It may be called again for the same draw data with another target of the same size, which is how an editor
		// screenshot re-renders the last UI frame (§8.13). Errors: those of RenderDrawData; InvalidState before the first
		// EndFrame.
		[[nodiscard]] Status Render(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer);

		// Switches imgui.ini: saves the settings to the current file (when set), points io.IniFilename at `iniFilePath`
		// (empty: none) and loads its settings (§8.11: the launcher's user:// file, then the project's). Errors: Io when the
		// directory cannot be created.
		[[nodiscard]] Status SetIniFilePath(const std::filesystem::path& iniFilePath);

		[[nodiscard]] const std::filesystem::path& GetIniFilePath() const;
		[[nodiscard]] ImGuiRenderer& GetRenderer();
		// The draw data of the last EndFrame; nullptr before the first.
		[[nodiscard]] ImDrawData* GetDrawData() const;

		// M24 (C24.1, E-VIEWPORTS; Docs/Decisions/0035-m24-contract.md decisions 17 and 18; stream V). EndFrame also calls
		// ImGui::UpdatePlatformWindows when platform windows are on (with GLFW's GLFW_CLIENT_API hint at GLFW_NO_API, so the
		// backend's new windows take a Vulkan surface), and EndInterruptedFrame leaves the platform windows as they are (their
		// targets are the recreation protocol's), only marking the frame as ended for them, as Dear ImGui's next frame requires.
		// Ok when platform windows are on. Errors: InvalidState when the specification did not ask for them; Unsupported from
		// CheckPlatformWindowSupport (Linux and macOS, a headless process); those of ImGuiPlatformWindows::Create, which leave
		// them off.
		[[nodiscard]] Status GetPlatformWindowSupport() const;
		// ImGuiPlatformWindows::Render for this frame; Ok, doing nothing, when platform windows are off. Application calls it
		// after the main window's submission and before FramePacer::EndFrame, on every frame that rendered the UI. Errors: those
		// of ImGuiPlatformWindows::Render, which the Application treats as ImGuiLayer::Render's.
		[[nodiscard]] Status RenderPlatformWindows(uint32_t frameSlot);
		// The platform windows; nullptr when they are off.
		[[nodiscard]] ImGuiPlatformWindows* GetPlatformWindows();
	private:
		// GLFW's null platform (headless processes): the layer is the platform backend itself (see ImGuiLayer.cpp).
		void InstallNullPlatform();
		void UninstallNullPlatform();
		// Per frame on the null platform: the display size and framebuffer scale from the window, as the GLFW backend
		// computes them on a native platform.
		void UpdateNullPlatform();
		// M24: Create's platform-window step, after the renderer: the ImGuiPlatformWindows and ImGuiConfigFlags_ViewportsEnable
		// when the specification asks, CheckPlatformWindowSupport allows and the GLFW backend is in use. Returns what
		// GetPlatformWindowSupport reports; on an error nothing of the step stays.
		[[nodiscard]] Status CreatePlatformWindows(GraphicsDevice& device, const ImGuiLayerSpecification& specification);
	private:
		std::filesystem::path m_IniFilePath;
		std::string m_IniFileName; // m_IniFilePath as UTF-8: the storage io.IniFilename points at
		Scope<ImGuiRenderer> m_Renderer;
		Scope<ImGuiPlatformWindows> m_PlatformWindows; // M24: with platform windows on, created after the renderer
		Status m_PlatformWindowSupport{};              // M24: GetPlatformWindowSupport's answer, from Create
		Window* m_Window = nullptr;                    // documented back-reference
		ImGuiContext* m_Context = nullptr;             // created by Create, destroyed by the destructor
		bool m_UsesGlfwBackend = false;                // the vendored GLFW backend is initialized (native platforms)
		bool m_UsesNullPlatform = false;               // InstallNullPlatform ran (GLFW's null platform)
	};

}
