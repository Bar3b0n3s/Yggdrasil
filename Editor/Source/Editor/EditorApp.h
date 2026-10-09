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
	// Autosave publishes owned CPU snapshots after commands and before GPU work. Fatal worker callbacks only claim
	// published bytes. Shutdown quiesces the hook, resets the writer lease, retires UI textures and releases the project.
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

		[[nodiscard]] Status InitializeEditor();
		[[nodiscard]] Status InitializeRendering();
		[[nodiscard]] Status InitializeUi();
		[[nodiscard]] Status SynchronizeProject();
		[[nodiscard]] Status BeforeProjectClose();
		[[nodiscard]] Status BeforePlay();
		[[nodiscard]] Status AfterSceneSaved();
		[[nodiscard]] Status UpdateHostServices();
		void PublishAutosave();
		void RefreshFatalSnapshot();
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
