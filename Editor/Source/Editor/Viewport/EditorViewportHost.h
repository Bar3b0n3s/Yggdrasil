#pragma once

#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Renderer/RenderSnapshot.h"

#include <imgui.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <span>

namespace Engine {

	// Owned by EditorApp. Texture keys are registered with ImGuiRenderer::AddTexture; retired with RemoveTexture when
	// replacing the target or closing a panel. The callback owner outlives EditorLayer. No NVRHI handle crosses EditorCore.
	struct EditorViewportImage
	{
		ImTextureID Texture = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
		uint64_t Generation = 0;    // target/camera/scene generation; invalidates pending clicks
		uint64_t FrameIndex = 0;    // actual RenderSnapshot/RenderContext source frame, never EditorUiState UI serial
		uint64_t SceneRevision = 0; // revision of the scene from which this image and its PickTable were extracted
		CameraData Camera{};        // camera copied from the same render snapshot as the displayed image
		bool Picking = false;       // rendered with RenderViewFlags::Picking: only such an image accepts RequestPick
	};

	struct EditorViewportClick
	{
		ViewportView View = ViewportView::Scene;
		EditorViewportPixel Pixel{};
		uint64_t FrameIndex = 0;    // copied from the displayed EditorViewportImage, not the current UI frame counter
		uint64_t SceneRevision = 0; // copied from the same displayed image, never sampled from a later live scene
		uint64_t Sequence = 0;      // newest user click only
		uint64_t ViewGeneration = 0;
		bool ExtendSelection = false;
	};

	// UI-to-renderer injected boundary. This adds no competing picking result or render flag enum.
	// M9 implementation owns AsyncPicker tickets/PickResult and updates target-aware editor selection after >=2 frames, if sequence,
	// scene revision AND view generation still match and the UUID still resolves. A miss uses nullopt; stale results
	// are discarded entirely; a stale miss must not clear selection. No blocking readback in Draw. Coordinates use
	// ToViewportPixel against the displayed image. Suppress submission while ImGuizmo is hovered or active.
	// FrameIndex/SceneRevision/Generation/PickTable are one immutable source identity, including resized or forced
	// offscreen frames. CompletedUiFrame counts UI construction and cannot supply a picking frame index.
	class EditorViewportHost
	{
	public:
		virtual ~EditorViewportHost() = default;
		[[nodiscard]] virtual EditorViewportImage GetImage(ViewportView view) const = 0;
		// Measured logical content rect/framebuffer scale, remembered for NEXT OnRender; zero area suppresses render/input
		// and retains last nonzero camera aspect. Fixed game resolution is letterboxed; rect excludes its bars.
		virtual void SetRectangle(ViewportView view, const EditorViewportRect& rectangle) = 0;
		// Unsupported without a rendered picking image. InvalidArgument outside current image, Conflict
		// stale source identity. Queue the copied click during Draw, with no GPU call. The host drains the queue from
		// Application::OnRenderSubmitted(frameIndex, submissionId), AFTER SceneRenderer::OnSubmitted and before any
		// overwrite/reuse, pairing only the same image/frame/table/generation. Frame-slot reuse includes the copy submission.
		// CPU tests inject a host; the production host adapts M9 AsyncPicker::Request/Poll. A changed camera, resize, scene
		// replacement or newer click invalidates old queued and pending picks before any selection mutation.
		[[nodiscard]] virtual Status RequestPick(const EditorViewportClick& click) = 0;
		// Build a copied scene-view snapshot with M9 Flags/SelectedEntities; Colliders uses AppendColliderDebugDraw.
		// M9 RenderQualitySettings remains owned by project rendering settings, not a new UI shadow setting.
		[[nodiscard]] virtual Status ConfigureSceneSnapshot(RenderSnapshot& snapshot, const EditorViewportOptions& options,
			std::span<const UUID> selection) = 0;
		// Whether the Game view owns game input (captured). The Game panel captures it with a click on its image and keeps
		// it while its window is focused, the image is held or the cursor is Locked; the panel releases it on Escape, focus
		// loss or hiding, and the host on native window blur, Play stop or invalidation. Losing it releases the held game
		// inputs of an ordinary session (never of a lockstep session, whose input only its owner injects) and restores the
		// Normal cursor. Lockstep shows the owner banner and rejects human time-control actions owned by another client.
		virtual void SetGameInputFocused(bool focused) = 0;
		// The state SetGameInputFocused last established, including a release by the host; false for hosts without game input.
		[[nodiscard]] virtual bool IsGameInputFocused() const { return false; }
	};

}
