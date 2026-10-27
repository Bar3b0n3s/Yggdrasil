#pragma once

#include "Editor/Viewport/EditorViewportHost.h"
#include "EditorCore/ShownSceneTracker.h"
#include "Engine/Core/Base.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Platform/Events.h"
#include "Engine/Renderer/SceneRenderer.h"

#include <array>
#include <optional>
#include <vector>

namespace Engine {

	class EditorContext;
	class GizmoController;
	class ImGuiRenderer;
	struct RenderContext;

	// EditorApp-owned adapter. All services are borrowed, main-thread-only and outlive this adapter.
	// Each view owns its renderer and ImGui registration; Retire withdraws registrations before GPU teardown.
	// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 26 and 33): the views' ImGui texture keys are one owner of
	// the device's GPU objects ("EditorHostViewports", registered by Initialize after the ImGui renderer and the views'
	// renderers, so a recovery releases it first). Its Release removes both keys (counted as withdrawals) and forgets the
	// images, the queued and pending picks and the recorded frame, which never completes; the renderers release and recreate
	// themselves (SceneRenderer, the placement rule's place 2), and the next Render registers the recreated images again.
	//
	// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 42 and 44): out-of-memory after startup fails only its request.
	// A view whose new size runs out of memory after its renderer's trim and retry (SceneRenderer::Resize's OutOfMemory) goes
	// on rendering at its previous size, which its panel shows scaled into the view's area and maps clicks through (the
	// image's size is the rendered size), with one Warn line per failed size; the size is asked for again only once the
	// view's size changes. A debug-view switch whose pipelines run out of memory (Render's OutOfMemory) sets the scene view's
	// debug-view setting back to the mode its renderer stayed on (SceneRenderer::GetRenderedDebugView), so the next frames do
	// not try again. A view's per-frame render that runs out of memory (a lazily created target or binding set) is trimmed
	// for once after the frame's submission (Submitted), and the next frame renders again.
	class EditorHostViewports final : public EditorViewportHost
	{
	public:
		EditorHostViewports(EditorContext& editor, GizmoController& gizmos);
		~EditorHostViewports() override;
		[[nodiscard]] Status Initialize(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache, ImGuiRenderer& imgui);
		// Renders every displayed view and notifies its renderer, even when another view fails: the first error is returned
		// after both views ran, a Gpu error before any other (M18, C18.4: out-of-memory of a lazily created target or binding
		// set, for which Submitted trims after the frame, never fatal; on a lost device the loss). A view that is not displayed
		// withdraws its image; when the shown scene changes while it is hidden, it also releases its renderer, whose binding
		// caches would otherwise keep the previous scene's GPU textures alive, and gets a new one when displayed again.
		[[nodiscard]] Status Render(RenderContext& context);
		// Notifies every view recorded in the frame of its submission and requests its queued pick. M18 (C18.4, decision 44):
		// then, when the frame's Render returned an out-of-memory error (IsGpuOutOfMemory: a Gpu error on a device that is
		// not lost), runs GraphicsDevice::TrimUnused once, which waits for that submission before any owner drops what it can
		// rebuild; a later frame whose Render succeeds trims nothing.
		[[nodiscard]] Status Submitted(uint64_t frameIndex, uint64_t submissionId);
		void Retire();
		void Invalidate();
		// The window was minimized (a zero framebuffer): withdraws both images and publishes zero extents. Until
		// SetAvailable, frames forced while minimized (editor.screenshot) still render the images for the UI capture
		// but never publish their extents, so the views stay unavailable to viewport.pick and stats.
		void SetUnavailable();
		// The window has a usable framebuffer again: the next render publishes the views' extents.
		void SetAvailable();
		// M19 --headless-game-view (EditorLaunchOptions::HeadlessGameView; Docs/Decisions/0030-m19-contract.md decision 11,
		// R-SOAK): from the next Render on, the Game view renders every frame as if its panel were displayed, at the project's
		// Window.Width x Window.Height (or the viewport state's game resolution when one is set), and publishes its extent and
		// statistics as a displayed view does (stats.get's "game"); it follows the shown scene (the play scene while playing)
		// and recreates its renderer like any view. EditorApp calls it once, after construction, for a headless editor started
		// with the option. Errors: InvalidState without a headless window (a windowed editor's panels decide).
		[[nodiscard]] Status ForceGameView();
		// Whether every shown view's image has the size it renders at (GetTargetSize: its panel's, or the forced Game view's
		// whatever its panel does, M18's merge with M19), or (M18, C18.4) the size it stays at because that size ran out of
		// memory. A forced Game view counts once it has rendered, also when its panel is open but not drawn (a headless
		// editor's default layout keeps the Game tab behind the Scene view), so editor.screenshot's fresh UI frame completes.
		[[nodiscard]] bool IsLayoutReady() const;
		// How many times registered view images were withdrawn outside a render (minimizing, retiring). A UI frame whose
		// draw data was built before the latest withdrawal references removed textures and must not be captured.
		[[nodiscard]] uint64_t GetTextureWithdrawalCount() const { return m_Withdrawals; }
		[[nodiscard]] Result<RenderSnapshot> Extract(ViewportView view, uint32_t width, uint32_t height);
		[[nodiscard]] SceneRenderer* GetRenderer(ViewportView view) const;
		[[nodiscard]] bool IsGameInputFocused() const override { return m_GameFocused; }
		// A native window event as game input in the play session's view coordinates (PlaySession.h), or nullopt when it is
		// not game input: the Game view does not own input, the event is no device input, or it is a button press outside
		// the displayed game image (letterbox bars included) while the cursor is not Locked. Cursor positions map through
		// the displayed image (fixed resolution, letterbox and framebuffer scale), unclamped so motion beyond the image and
		// Locked-cursor deltas stay continuous; releases always pass. Every cursor event updates the tracked pointer.
		[[nodiscard]] std::optional<Event> ToGameEvent(const Event& event);
		[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override;
		void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override;
		[[nodiscard]] Status RequestPick(const EditorViewportClick& click) override;
		[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot& snapshot, const EditorViewportOptions& options, std::span<const UUID> selection) override;
		// Capturing queues the pointer's current position as the game's cursor. Releasing restores the Normal cursor and,
		// for an ordinary (not lockstep) session without exclusive test/replay ownership, releases all held game input.
		// While the captured game has Locked the cursor, ImGui ignores the mouse (ImGuiConfigFlags_NoMouse), so presses at
		// the hidden cursor's position never reach other panels.
		void SetGameInputFocused(bool focused) override;
	private:
		struct PendingPick
		{
			PickTicket Ticket{};
			EditorViewportClick Click{};
		};
		struct View
		{
			Scope<SceneRenderer> Renderer{};
			EditorViewportImage Image{};
			glm::uvec2 RequestedSize = glm::uvec2(640, 360);
			nvrhi::ITexture* RegisteredTexture = nullptr; // renderer owns; ImGui holds its own retained handle
			const Scene* Source = nullptr;                // identity only; never dereferenced across frames
			RenderViewFlags Flags = RenderViewFlags::None;
			std::optional<EditorViewportClick> Queued{};
			std::vector<PendingPick> Pending{};
			uint64_t LatestSequence = 0;
			bool Recorded = false;
			bool HoldsSceneResources = false; // rendered since its renderer was created
			// M18 (C18.4, decision 44): the requested size whose Resize ran out of memory (0x0: none), not asked for again
			// until the requested size changes.
			glm::uvec2 FailedSize = glm::uvec2(0);
		};
		[[nodiscard]] static size_t Index(ViewportView view);
		// The size view `index` (0 the Scene view, 1 the Game view) renders at: the forced Game view's (ForceGameView: the game
		// resolution, or the project's deterministic window size) whatever its panel does; otherwise the size its open panel
		// asked for (0 x 0 for an open panel that is not drawn); nullopt for a view that is not shown (its panel closed, or no
		// project). RenderView and IsLayoutReady share it (M18's milestone review).
		[[nodiscard]] std::optional<glm::uvec2> GetTargetSize(size_t index) const;
		void Cancel(View& view);
		// The recreation owner's Release (see the class comment): no GPU call.
		void ReleaseGpuObjects();
		[[nodiscard]] Status Poll(View& view, uint64_t frameIndex);
		[[nodiscard]] Status RenderView(size_t index, RenderContext& context, bool sceneChanged);
		[[nodiscard]] bool IsCursorLocked() const;
	private:
		EditorContext& m_Editor;
		GizmoController& m_Gizmos;
		GraphicsDevice* m_Device = nullptr;            // borrowed from Initialize until Retire
		SceneRendererPipelines* m_Pipelines = nullptr; // borrowed from Initialize until Retire
		GpuResourceCache* m_Cache = nullptr;           // borrowed from Initialize until Retire
		ImGuiRenderer* m_ImGui = nullptr;
		std::array<View, 2> m_Views{};
		ShownSceneTracker m_ShownScenes{};
		EditorViewportRect m_GameRectangle{};
		std::optional<glm::vec2> m_Pointer{}; // the native cursor's last window position
		uint64_t m_Withdrawals = 0;
		bool m_Unavailable = false;
		bool m_GameFocused = false;
		bool m_TrimAfterSubmission = false;       // the last Render ran out of memory: Submitted trims (M18, C18.4 decision 44)
		bool m_GameForced = false;                // ForceGameView: the Game view renders whether or not its panel is displayed
		GpuRecreationRegistration m_Recreation{}; // from Initialize until Retire
	};

}
