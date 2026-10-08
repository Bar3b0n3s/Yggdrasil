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

	struct RenderContext;
	struct RenderSnapshot;
	struct ViewportScreenshotRequest;

	// The Editor executable's own options, next to the engine's (GetEngineCommandLineOptions) and EditorCore's
	// (GetEditorCommandLineOptions):
	//   --viewport-screenshot <path>  after the last frame of a --frames run, writes the viewport (the view drawn under the
	//                                 UI, see "Viewport" below) re-rendered at 640x360 through the viewport capture
	//                                 (Renderer/ViewportCapture.h) as a PNG
	//   --editor-screenshot <path>    after the last frame of a --frames run, writes the capture of the editor UI
	//                                 (ImGui/ImGuiScreenshot.h) as a PNG
	// They are thin callers of the captures that viewport.screenshot and editor.screenshot use too (CaptureView and
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
	// catalogues and exits; --bake-engine-assets fills the engine cooked cache with BakeEngineAssets over
	// engine://EngineAssets.json, before any editor state exists, and exits 0, or 1 when an entry failed to bake (entries this
	// build has no importer or generator for are skipped with a warning); --batch and --upgrade run a BatchRunner);
	// OnSafePoint pumps the server, advances a batch, honours a shutdown request and applies the play session's loop (the
	// project's FixedHz), time scale and throttle suspension to the frame loop (M7); OnFixedStep and OnUpdate drive the play session (EditorPlayController,
	// Docs/Decisions/0012-m7-decisions.md decision 3); OnUpdate drives asset hot reload
	// (EditorContext::Update with the frame clock's accumulated unscaled time, a ManualClock when headless); batch runs are
	// unthrottled (ThrottleHeadless off). The engine context mounts engine:// at <repo>/Resources and enginecache:// at
	// <repo>/bin/EngineCache, or at --engine-cache-dir (ApplicationSpecification::EngineResourcesDirectory and
	// EngineCacheDirectory; ADR 0010 decision 13), which every editor of the checkout shares, so any editor fills the engine
	// cooked cache on first use; tests pass --engine-cache-dir with a temporary directory. The server listens
	// (AutomationServerSpecification::Listen) when EditorLaunchOptions::ListensForAutomation(headless): with --automation, or
	// headless unless the run is one-shot, so a one-shot run never opens a port or writes a session file that an MCP
	// editor_launch could attach to.
	//
	// Viewport (Roadmap M7; Docs/Decisions/0012-m7-decisions.md decision 7): until the viewport panels (M10), the editor draws
	// one view of the scene renderer under its UI, filling the frame: in Edit mode the open edit scene through the
	// scene-view camera (EditorContext::GetSceneViewCamera; TransformSystem::Update first, no interpolation); while playing
	// the play session's game view (its last RenderExtraction, at the window's framebuffer size, PlaySession::SetViewSize);
	// while simulating the play scene through the scene-view camera (PlaySession::ExtractView); without a scene the empty
	// view (the default clear colour). OnUpdate extracts the view after the frame phase and OnRender renders it with the
	// editor's SceneRenderer and blits LdrColor into the frame's target (BlitPass, whose pipeline is created for the
	// target's format on the first frame that renders into it, like ImGui's, §8.12). A failed extraction (the frame then
	// shows the empty view) or a draw the renderer skipped (the rest is shown) is logged once per run of failing frames.
	//
	// Screenshots (§8.13; Docs/Decisions/0009-m5-decisions.md decisions 16 and 33, 0012 decision 9): with a device,
	// OnInitialize creates the editor's GPU objects once the EditorContext exists (its asset manager feeds them): the
	// GpuResourceCache, the SceneRendererPipelines every view shares (§8.5), the ViewportCapture over them and the viewport's
	// SceneRenderer; a Gpu error creating them is FatalError(OutOfMemory) (§8.14 item 7), any other error fails
	// initialization. The server receives the two captures (ScreenshotCaptures: CaptureView, which waits for the asset
	// manager (AssetManager::WaitIdle) and renders a snapshot through the ViewportCapture, and CaptureEditorUi) for
	// viewport.screenshot and editor.screenshot, and a std::system_error escaping a method is mapped like the
	// frame-boundary catch (RaiseVulkanError), so a GPU error inside a screenshot ends the process instead of becoming an
	// Internal response on a lost device. With --renderer none the server gets no captures and both methods are Unsupported.
	// OnShutdown destroys the GPU objects after the server and before the editor (the scene renderers, then the pipelines,
	// then the cache, §8.14 item 4).
	//
	// A failed OnInitialize (an invalid or locked project, a missing batch file, a server that cannot start, GPU objects that
	// cannot be created) runs OnShutdown itself before it returns the error, because Application::Run then destroys the
	// engine context and its device without calling OnShutdown: the server, the GPU objects, the editor and the hook are
	// released while the device still exists, and Run returns ExitCode::InitFailed (3).
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
		void OnFixedStep(const SimStep& step) override;
		void OnUpdate(const FrameTime& frame) override;
		void OnRender(RenderContext& context) override;
		void OnImGuiRender() override;

		// OnInitialize's work after the hook: the EditorCore state (State::Initialize), then the GPU objects with a device
		// (InitializeRendering). On failure OnInitialize releases what it built (OnShutdown).
		[[nodiscard]] Status InitializeEditor();
		// The GPU objects of the viewport and the screenshots (see "Screenshots" above), over the editor's asset manager.
		[[nodiscard]] Status InitializeRendering();
		// OnUpdate's extraction of this frame's view at `width` x `height` (Rendering::ViewSnapshot), or, in Play mode, the
		// choice of the session's last extraction; a failure is logged and leaves the empty view.
		void PrepareViewportView(uint32_t width, uint32_t height);
		// The viewport's view (see "Viewport" above) extracted for `width` x `height` (both >= 1); the empty view without
		// a scene. Errors: those of ExtractRenderSnapshot and PlaySession::ExtractView.
		[[nodiscard]] Result<RenderSnapshot> ExtractViewportView(uint32_t width, uint32_t height);
		// `snapshot` rendered through the viewport capture at request.Width x request.Height, at full size, after
		// AssetManager::WaitIdle (§8.13). Errors: Unsupported without a device (--renderer none); those of
		// ViewportCapture::Capture.
		[[nodiscard]] Result<Image> CaptureView(const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request);
		// The viewport re-rendered at width x height (ExtractViewportView, then CaptureView), at full size. Errors: those of
		// both.
		[[nodiscard]] Result<Image> CaptureViewport(uint32_t width, uint32_t height);
		// The editor UI's last frame re-rendered at its framebuffer size (CaptureImGuiScreenshot). Errors: Unsupported without
		// a device or ImGui (--renderer none); those of CaptureImGuiScreenshot.
		[[nodiscard]] Result<Image> CaptureEditorUi();
		// Writes the screenshots the options ask for; false when one failed (logged).
		[[nodiscard]] bool WriteScreenshots();
	private:
		// The launch options, the EditorContext, the AutomationServer and a batch or upgrade run (EditorApp.cpp).
		struct State;
		// The GPU objects of the viewport and the screenshots, and the view extracted for the frame (EditorApp.cpp).
		struct Rendering;
	private:
		EditorAppOptions m_Options;
		Scope<State> m_State;
		Scope<Rendering> m_Rendering; // with a device, from OnInitialize to OnShutdown
	};

	// The editor's ApplicationFactory: parses the engine, EditorCore and EditorAppOptions options (GetEngineCommandLineOptions,
	// GetEditorCommandLineOptions, ParseEditorLaunchOptions), names the application ENGINE_PRODUCT_NAME (§4.4), registers the
	// editor's automation types, turns ImGui on with ImGuiIniPath "Editor/imgui.ini" and turns the headless throttle off for
	// --batch and --upgrade (§4.2). Errors: InvalidArgument for a bad command line, including any positional argument (the
	// editor declares none), the option combinations ParseEditorLaunchOptions rejects, a screenshot option without --frames,
	// or with --frames 1 for --editor-screenshot.
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
