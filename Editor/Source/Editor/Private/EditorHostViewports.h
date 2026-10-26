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
	class EditorHostViewports final : public EditorViewportHost
	{
	public:
		EditorHostViewports(EditorContext& editor, GizmoController& gizmos);
		~EditorHostViewports() override;
		[[nodiscard]] Status Initialize(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache, ImGuiRenderer& imgui);
		// Renders every displayed view and notifies its renderer, even when another view fails: the first error is returned
		// after both views ran (a Gpu error at once, since the host ends the process for it). A view that is not displayed
		// withdraws its image; when the shown scene changes while it is hidden, it also releases its renderer, whose binding
		// caches would otherwise keep the previous scene's GPU textures alive, and gets a new one when displayed again.
		[[nodiscard]] Status Render(RenderContext& context);
		[[nodiscard]] Status Submitted(uint64_t frameIndex, uint64_t submissionId);
		void Retire();
		void Invalidate();
		// The window was minimized (a zero framebuffer): withdraws both images and publishes zero extents. Until
		// SetAvailable, frames forced while minimized (editor.screenshot) still render the images for the UI capture
		// but never publish their extents, so the views stay unavailable to viewport.pick and stats.
		void SetUnavailable();
		// The window has a usable framebuffer again: the next render publishes the views' extents.
		void SetAvailable();
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
		};
		[[nodiscard]] static size_t Index(ViewportView view);
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
		GpuRecreationRegistration m_Recreation{}; // from Initialize until Retire
	};

}
