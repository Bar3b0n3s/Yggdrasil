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
	//   --viewport-screenshot <path>  after the last frame of a --frames run, writes the scene view
	//                                 re-rendered at 640x360 through the viewport capture
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

	// Editor host: owns EditorCore services, launcher/panels, independent scene/game images and automation.
	// Autosave publishes owned CPU snapshots once per safe point, after commands and before the frame's GPU work, and
	// before request-driven captures. Fatal worker callbacks only claim published bytes. A window close with unsaved
	// scene changes asks Save / Don't save / Cancel first (EditorHostClose), or closes at once, keeping a final recovery, when
	// no UI can show the prompt. Shutdown saves the UI layout (open panels included) while the project's lock is held and
	// then stops saving it, quiesces the hook, resets the writer lease, retires UI textures and releases the project.
	// Render geometry measured in UI frame N applies to N+1. Picking copies the displayed identity after submission.
	class EditorApp final : public Application
	{
	public:
		EditorApp(ApplicationSpecification specification, EditorAppOptions options);
		~EditorApp() override;
	private:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
		void OnSafePoint() override;
		void OnEvent(Event& event) override;
		void OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId) override;
		void OnFixedStep(const SimStep& step) override;
		void OnUpdate(const FrameTime& frame) override;
		void OnRender(RenderContext& context) override;
		void OnImGuiRender() override;
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 29): before the recovery releases anything, the autosave
		// its fatal hook would write: the committed scene published, then written (Autosave::WriteFatalSnapshot), CPU only.
		// The play session goes on untouched.
		void OnDeviceLost(const GpuDeviceLossReport& report) override;
		// After a recovery: ImGui's last draw data names textures the recovery released, so a fresh UI frame replaces it
		// before an editor screenshot can capture it (RequestFreshUiFrame).
		void OnDeviceRecovered(const GpuDeviceLossReport& report) override;

		[[nodiscard]] Status InitializeEditor();
		[[nodiscard]] Status InitializeRendering();
		[[nodiscard]] Status InitializeUi();
		[[nodiscard]] Status SynchronizeProject();
		[[nodiscard]] Status BeforeProjectClose();
		[[nodiscard]] Status BeforePlay();
		[[nodiscard]] Status AfterSceneSaved();
		[[nodiscard]] Status UpdateHostServices();
		// Autosave::Publish, its failure logged once per distinct error (ReportAutosave).
		void PublishAutosave();
		void ReportAutosave(const Status& status);
		// Publishes the current committed scene before FatalError, whose installed hook writes it once.
		void RefreshFatalSnapshot();
		// Asks for a complete UI frame at the next render, also while minimized (RequestOffscreenUiFrame), and keeps asking
		// until one completes with the views' layout ready (OnRenderSubmitted): editor.screenshot's request, and the
		// recovery's.
		[[nodiscard]] Status RequestFreshUiFrame();
		void NoteShownScene();
		void CollectStaleMirrors();
		// The scene view extracted for `width` x `height` (both >= 1); the empty view without
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
		// Writes the screenshots the options ask for; false when one failed (logged). `afterDeviceRecovery`: called from
		// OnDeviceRecovered after a device loss on the run's last frame, where the viewport is captured afresh on the recovered
		// device and the editor UI capture fails, since no UI frame was drawn on it (M18's milestone review).
		[[nodiscard]] bool WriteScreenshots(bool afterDeviceRecovery = false);
	private:
		// The launch options, the EditorContext, the AutomationServer, a batch or upgrade run and an --export run, whose
		// Exporter is polled once per safe point and whose outcome writes --export-report and sets the exit code
		// (EditorApp.cpp; EditorCommandLine.h).
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
	// or with --frames 1 for --editor-screenshot, and --help (RunEditor answers it before any application exists).
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

	// The editor's main (M16 friction F20): with --help or -h among arguments that otherwise parse (ExpandEditorShortOptions,
	// ParseEditorLaunchOptions), writes the usage, "Usage: Editor [options]" and every option of the engine, EditorCore and
	// the Editor executable with its one-line description (CommandLine::FormatUsage), to standard output and returns
	// ExitCode::Success without creating the application or the process context (no log file, no user-data directory, no
	// Vulkan loader); ExitCode::Failed when standard output cannot be written. Otherwise RunApplication(argc, argv,
	// &CreateEditorApp), so an unknown option stays a usage error (exit code 2).
	[[nodiscard]] int RunEditor(int argc, char** argv);

}
