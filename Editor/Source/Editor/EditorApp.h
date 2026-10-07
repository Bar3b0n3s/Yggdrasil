#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/Image.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace Engine {

	class ViewportCapture;

	// The Editor executable's own options, next to the engine's (GetEngineCommandLineOptions) and EditorCore's
	// (GetEditorCommandLineOptions):
	//   --viewport-screenshot <path>  after the last frame of a --frames run, writes the viewport capture (640x360,
	//                                 Renderer/ViewportCapture.h) as a PNG
	//   --editor-screenshot <path>    after the last frame of a --frames run, writes the capture of the editor UI
	//                                 (ImGui/ImGuiScreenshot.h) as a PNG
	// They are thin callers of the captures that viewport.screenshot and editor.screenshot use too (CaptureViewport and
	// CaptureEditorUi, injected into the automation server as ScreenshotCaptures).
	struct EditorAppOptions
	{
		std::filesystem::path ViewportScreenshotPath{};
		std::filesystem::path EditorScreenshotPath{};
	};

	// The editor application (Architecture §12). The editor executable runs the frame loop with a window, or headless with
	// --headless, and renders through the engine's frame (RendererMode::Vulkan by default; --renderer none for logic-only
	// runs, §13.9). Its UI is Dear ImGui's demo window until the panels arrive (M10), with imgui.ini under user://Editor/
	// (§8.11); its context's type registry holds the automation structs (RegisterEditorMethodTypes).
	//
	// It hosts EditorCore (Docs/Decisions/0008-m4-decisions.md decision 14): the factory parses the editor options
	// (GetEditorCommandLineOptions, ParseEditorLaunchOptions); OnInitialize creates the EditorContext, opens --project (exit 3
	// when it is locked or invalid), creates the AutomationServer and handles the one-shot modes (--dump-reference writes the
	// catalogues and exits; --batch and --upgrade run a BatchRunner); OnSafePoint pumps the server, advances a batch and
	// honours a shutdown request; batch runs are unthrottled (ThrottleHeadless off). The server listens
	// (AutomationServerSpecification::Listen) when EditorLaunchOptions::ListensForAutomation(headless): with --automation, or
	// headless unless the run is one-shot, so a one-shot run never opens a port or writes a session file that an MCP
	// editor_launch could attach to.
	//
	// Screenshots (§8.13; Docs/Decisions/0009-m5-decisions.md decisions 16 and 33): with a device, OnInitialize creates the
	// editor's ViewportCapture, which owns the viewport pipeline from startup (§8.12); a Gpu error creating it is
	// FatalError(OutOfMemory) (§8.14 item 7), any other error fails initialization. The server receives the two captures
	// (ScreenshotCaptures: CaptureViewport and CaptureEditorUi) for viewport.screenshot and editor.screenshot, and a
	// std::system_error escaping a method is mapped like the frame-boundary catch (RaiseVulkanError), so a GPU error inside
	// a screenshot ends the process instead of becoming an Internal response on a lost device. With --renderer none the
	// server gets no captures and both methods are Unsupported. OnShutdown destroys the capture after the server.
	//
	// A failed OnInitialize (an invalid or locked project, a missing batch file, a server that cannot start, a capture that
	// cannot be created) runs OnShutdown itself before it returns the error, because Application::Run then destroys the
	// engine context and its device without calling OnShutdown: the server, the editor, the capture's GPU objects and the
	// hook are released while the device still exists, and Run returns ExitCode::InitFailed (3).
	//
	// Fatal errors (§4.6, §8.14 item 6): the editor installs its fatal-error hook (ProcessContext::SetFatalErrorHook) in
	// OnInitialize and removes it in OnShutdown. The hook is the place of the autosave (EditorCore/Autosave, M10); until then
	// it logs "Editor fatal-error hook (<kind>): nothing is autosaved before EditorCore/Autosave (M10)" at Warn, with the
	// FatalErrorKind name, which the crash report's last log lines then show before the report is written. It may run on
	// any thread, so it reads no editor state.
	//
	// Screenshot options: the capture runs in OnUpdate of the run's last frame (FrameTime::FrameIndex + 1 == MaxFrames),
	// so --editor-screenshot shows the UI of the frame before it (ImGuiLayer::GetDrawData), which needs --frames >= 2. A
	// failed capture or write is logged at Error level and ends the run with ExitCode::Failed (1).
	class EditorApp final : public Application
	{
	public:
		EditorApp(ApplicationSpecification specification, EditorAppOptions options);
		~EditorApp() override;
	private:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
		void OnSafePoint() override;
		void OnUpdate(const FrameTime& frame) override;
		void OnImGuiRender() override;

		// OnInitialize's work after the hook: the viewport capture with a device, then the EditorCore state
		// (State::Initialize). On failure OnInitialize releases what it built (OnShutdown).
		[[nodiscard]] Status InitializeEditor();
		// The viewport re-rendered at width x height (ViewportCapture::Capture), at full size. Errors: Unsupported without a
		// device (--renderer none); those of ViewportCapture::Capture.
		[[nodiscard]] Result<Image> CaptureViewport(uint32_t width, uint32_t height);
		// The editor UI's last frame re-rendered at its framebuffer size (CaptureImGuiScreenshot). Errors: Unsupported without
		// a device or ImGui (--renderer none); those of CaptureImGuiScreenshot.
		[[nodiscard]] Result<Image> CaptureEditorUi();
		// Writes the screenshots the options ask for; false when one failed (logged).
		[[nodiscard]] bool WriteScreenshots();
	private:
		// The launch options, the EditorContext, the AutomationServer and a batch or upgrade run (EditorApp.cpp).
		struct State;
	private:
		EditorAppOptions m_Options;
		Scope<ViewportCapture> m_ViewportCapture; // with a device, from OnInitialize to OnShutdown
		Scope<State> m_State;
	};

	// The editor's ApplicationFactory: parses the engine, EditorCore and EditorAppOptions options (GetEngineCommandLineOptions,
	// GetEditorCommandLineOptions, ParseEditorLaunchOptions), names the application ENGINE_PRODUCT_NAME (§4.4), registers the
	// editor's automation types, turns ImGui on with ImGuiIniPath "Editor/imgui.ini" and turns the headless throttle off for
	// --batch and --upgrade (§4.2). Errors: InvalidArgument for a bad command line, including any positional argument (the
	// editor declares none), the option combinations ParseEditorLaunchOptions rejects, a screenshot option without --frames,
	// or with --frames 1 for --editor-screenshot.
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
