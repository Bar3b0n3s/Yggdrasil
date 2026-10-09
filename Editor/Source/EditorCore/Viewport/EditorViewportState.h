#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Scene/RenderExtraction.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <optional>

namespace Engine {

	enum class ViewportView : uint8_t;

	// Editor preferences, not renderer flags. The M9 adapter maps these to RenderViewFlags on a copied snapshot:
	// EditorOverlays always for the scene view; Grid/Colliders/Icons/Wireframe from these members, Selection when selected.
	// Gizmos controls the ImGuizmo interaction. The game view never inherits scene-view overlays.
	struct EditorViewportOptions
	{
		bool Grid = true;
		bool Gizmos = true;
		bool Colliders = false;
		bool Icons = true;
		bool Wireframe = false;
	};

	// UI display rectangle in logical screen coordinates; ImageSize is the displayed texture's actual pixel size.
	// Letterbox bars are excluded. Geometry observed in UI frame N is rendered in frame N+1 (OnRender precedes ImGui).
	struct EditorViewportRect
	{
		glm::vec2 Min = glm::vec2(0.0f);
		glm::vec2 Size = glm::vec2(0.0f);
		glm::uvec2 ImageSize = glm::uvec2(0);
		bool Focused = false;
		bool Hovered = false;
	};

	struct EditorViewportPixel
	{
		uint32_t X = 0;
		uint32_t Y = 0;
	};

	// Pure CPU hit mapping, top-left origin, floor to the displayed image's pixels, right/bottom exclusive.
	// nullopt for zero/non-finite rectangles, out-of-image coordinates or a hidden/minimized view; never divide by zero.
	[[nodiscard]] std::optional<EditorViewportPixel> ToViewportPixel(const EditorViewportRect& rectangle, const glm::vec2& point);

	// Main-thread editor state owned by EditorContext. Camera and options are session state: changing them never changes
	// scene revision, dirty state or history. Headless and --renderer none hosts still own this state for automation.
	class EditorViewportState
	{
	public:
		[[nodiscard]] const ExplicitRenderCamera& GetCamera() const { return m_Camera; }
		[[nodiscard]] const EditorViewportOptions& GetOptions() const { return m_Options; }
		[[nodiscard]] RenderDebugView GetDebugView() const { return m_DebugView; }
		// Last nonzero RENDERED scene size, default 640x360; frame-to-selection uses this aspect even headless.
		[[nodiscard]] glm::uvec2 GetSceneSize() const { return m_SceneSize; }
		// Current scene/game image extent, default 640x360 in a headless host. The host publishes the actual rendered
		// extent, never a requested panel size or a screenshot size. InvalidState when hidden/minimized (both zero).
		// InvalidArgument for an invalid view. Does not substitute the last usable camera aspect for an unavailable image.
		[[nodiscard]] Result<glm::uvec2> GetPixelSize(ViewportView view) const;
		// Main-thread host publication after rendering, or both zero when a view becomes unavailable. InvalidArgument for
		// an invalid view, one zero dimension or a side above 8192. Updates the scene's last usable aspect when nonzero;
		// a fixed game resolution preference takes effect here only after the matching image has actually been rendered.
		[[nodiscard]] Status SetPixelSize(ViewportView view, const glm::uvec2& size);
		// Both zero means free game resolution. Otherwise [1,8192] each, letterboxed in the available panel.
		[[nodiscard]] glm::uvec2 GetGameResolution() const { return m_GameResolution; }
		// Zero/minimized dimensions are ignored and preserve the last usable aspect. Main thread, host after render.
		void SetSceneSize(const glm::uvec2& size);
		// InvalidArgument for only one zero dimension or either side above 8192; leaves the previous choice on failure.
		[[nodiscard]] Status SetGameResolution(const glm::uvec2& size);

		// Validate the whole candidate before replacing state. InvalidArgument for non-finite numbers, coincident position
		// and target, vertical viewing direction parallel to +Y, invalid projection/FOV/clip range or nonpositive ortho size.
		[[nodiscard]] Status SetCamera(const ExplicitRenderCamera& camera);
		// A full replacement; viewport.setOptions merges only the members present in the request before calling this.
		void SetOptions(const EditorViewportOptions& options);
		// InvalidArgument for an enumerator outside RenderDebugViewCount; no scene mutation.
		[[nodiscard]] Status SetDebugView(RenderDebugView view);
	private:
		ExplicitRenderCamera m_Camera{};
		EditorViewportOptions m_Options{};
		RenderDebugView m_DebugView = RenderDebugView::Lit;
		glm::uvec2 m_SceneSize = glm::uvec2(640, 360);
		glm::uvec2 m_ScenePixels = glm::uvec2(640, 360);
		glm::uvec2 m_GamePixels = glm::uvec2(640, 360);
		glm::uvec2 m_GameResolution = glm::uvec2(0);
	};

}
