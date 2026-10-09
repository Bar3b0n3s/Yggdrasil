#pragma once

#include "Editor/Viewport/EditorViewportHost.h"
#include "Engine/Core/Base.h"
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
	class EditorHostViewports final : public EditorViewportHost
	{
	public:
		EditorHostViewports(EditorContext& editor, GizmoController& gizmos);
		~EditorHostViewports() override;
		[[nodiscard]] Status Initialize(GraphicsDevice& device, SceneRendererPipelines& pipelines, GpuResourceCache& cache, ImGuiRenderer& imgui);
		[[nodiscard]] Status Render(RenderContext& context);
		[[nodiscard]] Status Submitted(uint64_t frameIndex, uint64_t submissionId);
		void Retire();
		void Invalidate();
		void SetUnavailable();
		[[nodiscard]] bool IsLayoutReady() const;
		[[nodiscard]] Result<RenderSnapshot> Extract(ViewportView view, uint32_t width, uint32_t height);
		[[nodiscard]] SceneRenderer* GetRenderer(ViewportView view) const;
		[[nodiscard]] bool IsGameInputFocused() const { return m_GameFocused; }
		[[nodiscard]] EditorViewportImage GetImage(ViewportView view) const override;
		void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) override;
		[[nodiscard]] Status RequestPick(const EditorViewportClick& click) override;
		[[nodiscard]] Status ConfigureSceneSnapshot(RenderSnapshot& snapshot, const EditorViewportOptions& options, std::span<const UUID> selection) override;
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
		};
		[[nodiscard]] static size_t Index(ViewportView view);
		void Cancel(View& view);
		[[nodiscard]] Status Poll(View& view, uint64_t frameIndex);
	private:
		EditorContext& m_Editor;
		GizmoController& m_Gizmos;
		ImGuiRenderer* m_ImGui = nullptr;
		std::array<View, 2> m_Views{};
		bool m_GameFocused = false;
	};

}
