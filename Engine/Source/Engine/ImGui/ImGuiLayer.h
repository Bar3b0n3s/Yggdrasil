#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <imgui.h>
#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <filesystem>

// Dear ImGui for an application (Architecture §8.11): the context, the vendored GLFW platform backend
// (ImGui_ImplGlfw_InitForVulkan, compiled in ImGui/ImGuiGlfwImplementation.cpp), the NVRHI renderer (ImGuiRenderer) and
// the imgui.ini location. Docking is on, multi-viewport off. The GLFW backend runs on GLFW's null platform in headless
// processes, so the full editor UI renders offscreen there (editor.screenshot, §8.13, §13.9).

namespace Engine {

	class GraphicsDevice;
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
	};

	// One per process at a time (Dear ImGui's current context is global; asserted). Not copyable or movable; main thread
	// only.
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
		// DestroyTextures on the renderer (the device must be idle: Application waits before), ImGui_ImplGlfw_Shutdown,
		// then ImGui::DestroyContext, which saves imgui.ini when an ini path is set.
		~ImGuiLayer();

		ImGuiLayer(const ImGuiLayer&) = delete;
		ImGuiLayer& operator=(const ImGuiLayer&) = delete;

		// Creates the context (docking enabled, viewports disabled, the ini path), initializes the GLFW backend on `window`
		// with its callbacks installed (they chain to the Window's own) and creates the ImGuiRenderer. `window`, `device`
		// and `pipelines` are documented back-references that must outlive the layer. Errors: InvalidState when another
		// ImGuiLayer exists; Io when the ini directory cannot be created; Unsupported when the GLFW backend cannot
		// initialize; those of ImGuiRenderer::Create. Nothing stays created on failure.
		[[nodiscard]] static Result<Scope<ImGuiLayer>> Create(Window& window, GraphicsDevice& device, PipelineFactory& pipelines,
			const ImGuiLayerSpecification& specification);

		// Starts a UI frame: ImGuiRenderer::BeginFrame(frameSlot), ImGui_ImplGlfw_NewFrame, then io.DeltaTime =
		// max(`deltaSeconds`, MinimumDeltaSeconds), then ImGui::NewFrame. Widgets may be submitted until EndFrame.
		// `deltaSeconds` (>= 0, finite; asserted) is the frame clock's time since the previous UI frame, which Application
		// accumulates over the frames the swapchain skipped. It replaces the backend's wall-clock delta, so headless runs on a
		// ManualClock render identical frames (the ImGuiDemo golden). Dear ImGui asserts a positive DeltaTime, and a
		// SystemClock's first delta is 0, hence the minimum.
		void BeginFrame(double deltaSeconds, uint32_t frameSlot);

		// ImGui::Render: finishes the frame's draw data.
		void EndFrame();

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
	private:
		std::filesystem::path m_IniFilePath;
		Scope<ImGuiRenderer> m_Renderer;
	};

}
