#pragma once

#include "Engine/App/Application.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <span>
#include <string>

namespace Engine {

	class ViewportCapture;

	// The editor's own options, next to the engine's (GetEngineCommandLineOptions):
	//   --viewport-screenshot <path>  after the last frame of a --frames run, writes ViewportCapture::Capture (640x360,
	//                                 Renderer/ViewportCapture.h) as a PNG
	//   --editor-screenshot <path>    after the last frame of a --frames run, writes CaptureImGuiScreenshot of the editor
	//                                 UI (ImGui/ImGuiScreenshot.h) as a PNG
	// They are thin callers of the screenshot capability, which viewport.screenshot and editor.screenshot call too once they
	// are registered after the M4/M5 merge; the golden test "ImGuiDemo" uses --editor-screenshot.
	struct EditorAppOptions
	{
		std::filesystem::path ViewportScreenshotPath{};
		std::filesystem::path EditorScreenshotPath{};
	};

	// The editor application (Architecture §12). In M5 it renders through the engine's frame (RendererMode::Vulkan by
	// default; --renderer none for logic-only runs) and shows Dear ImGui's demo window as its UI, with imgui.ini under
	// user://Editor/ (§8.11); EditorCore, automation and the panels arrive with their milestones.
	//
	// Fatal errors (§4.6, §8.14 item 6): the editor installs its fatal-error hook (ProcessContext::SetFatalErrorHook) in
	// OnInitialize and removes it in OnShutdown. The hook is the place of the autosave; until a project can be open
	// (EditorCore, M4; autosave, M10) it logs "Editor fatal-error hook (<kind>): no project is open, nothing to autosave"
	// at Warn, with the FatalErrorKind name, which the crash report's last log lines then show before the report is
	// written.
	//
	// Viewport capture (§8.13): with a device, OnInitialize creates the editor's ViewportCapture, which owns the viewport
	// pipeline from startup (§8.12) and serves --viewport-screenshot now and viewport.screenshot after the M4/M5 merge; a
	// Gpu error creating it is FatalError(OutOfMemory) (§8.14 item 7), any other error fails initialization. OnShutdown
	// destroys it.
	//
	// Screenshot options: the capture runs in OnUpdate of the run's last frame (FrameTime::FrameIndex + 1 == MaxFrames),
	// so --editor-screenshot shows the UI of the frame before it (ImGuiLayer::GetDrawData), which needs --frames >= 2. A
	// failed capture or write is logged at Error level and ends the run with ExitCode::Failed (1).
	class EditorApp final : public Application
	{
	public:
		EditorApp(ApplicationSpecification specification, EditorAppOptions options);

		[[nodiscard]] const EditorAppOptions& GetOptions() const { return m_Options; }
	protected:
		[[nodiscard]] Status OnInitialize() override;
		void OnShutdown() override;
		void OnUpdate(const FrameTime& frame) override;
		void OnImGuiRender() override;
	private:
		// Writes the requested screenshots; false when one failed (logged).
		[[nodiscard]] bool WriteScreenshots();
	private:
		EditorAppOptions m_Options;
		Scope<ViewportCapture> m_ViewportCapture; // with a device, from OnInitialize to OnShutdown
	};

	// The editor's ApplicationFactory: parses the engine options plus EditorAppOptions' options and names the application
	// ENGINE_PRODUCT_NAME (§4.4); turns ImGui on with ImGuiIniPath "Editor/imgui.ini". Errors: InvalidArgument for a bad
	// command line, including any positional argument (the editor declares none yet), a screenshot option without
	// --frames, or with --frames 1 for --editor-screenshot.
	[[nodiscard]] Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments);

}
