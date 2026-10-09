#pragma once

#include "EditorCore/Autosave/Autosave.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	class AutomationServer;
	class EditorActions;
	class EditorAutomationControls;
	class EditorContext;
	class EditorViewportHost;
	class GizmoController;
	class ReflectedEditController;
	class ThumbnailCache;
	struct ThumbnailRequest;

	enum class EditorRecoveryDecision : uint8_t
	{
		Accept,
		Decline
	};

	// An offer for the already-open project. Copies own all data. The host issues a nonzero process-monotonic Id and
	// binds it privately to that project's open epoch and current edit revision, even if another project reuses its path.
	struct EditorRecoveryOffer
	{
		uint64_t Id = 0;
		AutosaveRecoveryInfo Recovery{};
		bool DecisionPending = false;
		std::optional<Error> Failure{}; // last application failure, retained for the modal until retried/dismissed
	};

	// Explicit UI/CPU service injection, owned by the host; captures outlive EditorLayer. No second project.open call.
	// The host inspects with Autosave::FindRecovery at the safe point after open, while holding the project's lock.
	// The modal belongs to EditorLayer after the launcher disappears. Missing callbacks mean Unsupported recovery UI.
	struct EditorRecoveryServices
	{
		// Memory-only copied offer and pending/error state, nullopt when absent/declined/stale; no disk I/O in Draw.
		std::function<std::optional<EditorRecoveryOffer>()> GetOffer{};
		// Queue only: owns offer/decision, applies at the next safe point. InvalidArgument unknown decision/zero Id;
		// NotFound withdrawn/unknown offer; Conflict changed project/scene/revision; PermissionDenied read-only acceptance.
		// InvalidState when another decision is pending. The host, not caller-provided Recovery bytes, owns the offer record.
		// Repeat all identity/revision checks at execution before calling Autosave::Recover. Accept installs the scene once;
		// Decline only dismisses this offer for this open epoch, with no scene/history/source/autosave file change.
		// Pending decisions cancel on close; completion/failure appears in the modal/Console and never silently dismisses it.
		std::function<Status(const EditorRecoveryOffer&, EditorRecoveryDecision)> QueueDecision{};
	};

	struct EditorAutomationPreferenceState
	{
		bool Allowed = false;
		bool Pending = false;
		std::optional<Error> Failure{};
	};

	struct EditorAutomationPreferenceServices
	{
		// Memory-only snapshot; startup loads preferences and safe points publish completion or failure.
		std::function<EditorAutomationPreferenceState()> GetState{};
		// Queue only, no I/O in Draw. The host calls Controls.SetAllowAiAutomation at the next safe point.
		// InvalidState if another change is pending; completion clears Pending and publishes Allowed or Failure.
		std::function<Status(bool)> QueueChange{};
	};

	// The host owns this service bundle for EditorLayer's lifetime. Its references are documented back-references to
	// services that outlive it; individual panels borrow it only for Draw. CPU models live in EditorCore, and no panel
	// owns the scene or retains a component/asset pointer. Errors remain visible in the initiating panel and Console.
	struct EditorPanelContext
	{
		EditorContext& Editor;
		AutomationServer& Automation;
		EditorActions& Actions;
		EditorAutomationControls& AutomationControls;
		EditorViewportHost& Viewports;
		ReflectedEditController& InspectorEdits;
		ThumbnailCache& Thumbnails;
		GizmoController& Gizmos; // shared with the scene viewport host; preview is scene-view only
		// Memory-only lookup of the registered thumbnail texture for this exact bound request; 0 when absent.
		// The host pumps/uploads outside Draw and retires registrations before the cache binding is reset.
		std::function<uint64_t(const ThumbnailRequest&)> FindThumbnailTexture{};
		// Drain copied OS-drop paths for the current project epoch. No I/O; the panel queues asset.import.
		std::function<std::vector<std::filesystem::path>()> TakeContentDrops{};
		// Queue only: valid handle plays via AudioPreview; null stops. The host checks the project epoch at execution,
		// calls Play/Stop at a safe point, and publishes asynchronous failures in Console. No asset loading in Draw.
		std::function<Status(AssetHandle)> QueueAudioPreview{};
		EditorRecoveryServices Recovery{};
		EditorAutomationPreferenceServices AutomationPreferences{};
		// OS drops are copied as native paths; asset.import validates them and writes/provenance through existing commands.
		// External editor launch uses Process with argument lists, never a shell, and propagates Io/NotFound.
		std::function<Status(const std::filesystem::path&, uint32_t)> OpenSource{};
	};

}
