#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <glm/glm.hpp>

#include <cstdint>
#include <span>

namespace Engine {

	class EditorContext;
	class Scene;

	enum class GizmoOperation : uint8_t
	{
		Translate,
		Rotate,
		Scale
	};
	enum class GizmoSpace : uint8_t
	{
		Local,
		World
	};

	struct GizmoSettings
	{
		GizmoOperation Operation = GizmoOperation::Translate;
		GizmoSpace Space = GizmoSpace::World;
		bool Snap = false;
		float TranslationSnap = 0.5f; // metres
		float RotationSnap = 15.0f;   // degrees
		float ScaleSnap = 0.1f;
	};

	// CPU logic only; ImGuizmo calls live in Editor/Viewport/GizmoOverlay. Main thread, non-copyable.
	// Stores UUIDs and an owned preview scene copy, never a live component reference. context outlives the controller.
	// Begin captures the base revision and scene; Update edits only the preview, used by the scene viewport's extraction.
	// End applies the final poses with one short SceneEdit. In play/simulate it targets only the runtime scene and
	// returns index 0 (transient, no history). No live tracker spans frames, so agent commands can interleave.
	// An intervening agent/UI edit, undo, scene swap or play transition discards the preview with Conflict before writing.
	class GizmoController
	{
	public:
		explicit GizmoController(EditorContext& context);
		~GizmoController();
		GizmoController(const GizmoController&) = delete;
		GizmoController& operator=(const GizmoController&) = delete;

		// Capture the selection's root-most entities (selected descendants follow ancestors once), pivot and revision.
		// InvalidState without a scene or during a drag; PermissionDenied read-only; NotFound unknown
		// UUID; InvalidArgument empty selection/invalid settings/non-finite or singular pivot. No scene change yet.
		[[nodiscard]] Status Begin(std::span<const UUID> entities, const glm::mat4& worldPivot, const GizmoSettings& settings);
		// Absolute manipulated world pivot since Begin, not a per-frame delta. Converts through each parent's inverse;
		// all decompositions are validated before any write. Validation for shear/non-invertible parent or bad scale.
		// Changes only the preview. Failed validation keeps the prior preview pose and the drag can continue.
		[[nodiscard]] Status Update(const glm::mat4& worldPivot);
		// Merges the whole gesture into one undo entry at release, 0 for no movement; InvalidState idle, Conflict stale.
		[[nodiscard]] Result<uint64_t> End();
		// ESC/focus loss discards the preview. No-op idle; no live mutation, history entry or rollback of another edit.
		void Cancel();
		[[nodiscard]] bool IsDragging() const;
		// Borrowed until Update/End/Cancel; nullptr idle. For scene-viewport extraction only, never simulation/autosave.
		[[nodiscard]] const Scene* GetPreviewScene() const;
	};

}
