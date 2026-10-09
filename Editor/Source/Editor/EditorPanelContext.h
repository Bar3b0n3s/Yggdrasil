#pragma once

#include "EditorCore/Autosave/Autosave.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace Engine {

	class AutomationServer;
	class EditorActions;
	class EditorAutomationControls;
	class EditorContext;
	class EditorViewportHost;
	class ReflectedEditController;
	class ThumbnailCache;

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
		EditorRecoveryServices Recovery{};
		// OS drops are copied as native paths; asset.import validates them and writes/provenance through existing commands.
		// External editor launch uses Process with argument lists, never a shell, and propagates Io/NotFound.
		std::function<Status(const std::filesystem::path&, uint32_t)> OpenSource{};
	};

}
