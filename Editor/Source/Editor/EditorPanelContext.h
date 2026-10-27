#pragma once

#include "Editor/FileDialog.h"
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
		AutosaveRecoveryInfo Recovery{}; // the newest offered scene
		// Every offered scene's name, oldest first: one offer covers every scene with a recoverable generation, and Accept
		// recovers them all (Docs/Decisions/0031-m20-contract.md decision 15).
		std::vector<std::string> SceneNames{};
		bool DecisionPending = false;
		std::optional<Error> Failure{}; // last application failure, retained for the modal until retried/dismissed
	};

	// Explicit UI/CPU service injection, owned by the host; captures outlive EditorLayer. No second project.open call.
	// The host inspects with Autosave::FindRecoveries at the safe point after open, while holding the project's lock.
	// The modal belongs to EditorLayer after the launcher disappears. Missing callbacks mean Unsupported recovery UI.
	struct EditorRecoveryServices
	{
		// Memory-only copied offer and pending/error state, nullopt when absent/declined/stale; no disk I/O in Draw.
		std::function<std::optional<EditorRecoveryOffer>()> GetOffer{};
		// Queue only: owns offer/decision, applies at the next safe point. InvalidArgument unknown decision/zero Id;
		// NotFound withdrawn/unknown offer; Conflict changed project/scene/revision; PermissionDenied read-only acceptance.
		// InvalidState when another decision is pending. The host, not caller-provided Recovery bytes, owns the offer record.
		// Repeat all identity/revision checks at execution before calling Autosave::Recover. Accept installs each scene once;
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

	// The external source editor preference (EditorPreferences::ExternalEditorExecutable and ExternalEditorArguments in
	// user://Editor.json), as the host last loaded or wrote it.
	struct EditorExternalEditorState
	{
		std::string Executable{};             // empty: no external editor configured
		std::vector<std::string> Arguments{}; // argv entries; {path} and {line} are substituted at launch, never by a shell
		bool Pending = false;                 // a queued change waits for the next safe point
		std::optional<Error> Failure{};       // the last change's failure, until another change is queued
	};

	struct EditorExternalEditorServices
	{
		// Memory-only snapshot; no I/O in Draw.
		std::function<EditorExternalEditorState()> GetState{};
		// Queue only, no I/O in Draw. The host writes the preference at the next safe point (ReadEditorPreferences, then
		// WriteEditorPreferences, so RecentProjects and unknown members survive) and publishes the new snapshot or Failure.
		// InvalidState while a change is pending; InvalidArgument for text that is not valid UTF-8 or holds NUL, and for
		// arguments without a {path} entry (BuildEditorSourceProcessSpecification's rule).
		std::function<Status(std::string, std::vector<std::string>)> QueueChange{};
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
		// Drain copied OS-drop paths for the current project epoch. No I/O; the panel queues asset.import. A drop with a
		// project open also opens and focuses the Content panel (EditorLayer::RevealPanel), so it gives immediate feedback:
		// the panel imports into its shown folder or asks for a destination when it was not drawn on the previous frame.
		std::function<std::vector<std::filesystem::path>()> TakeContentDrops{};
		// Queue only: valid handle plays via AudioPreview; null stops. The host checks the project epoch at execution,
		// calls Play/Stop at a safe point, and publishes asynchronous failures in Console. No asset loading in Draw.
		std::function<Status(AssetHandle)> QueueAudioPreview{};
		EditorRecoveryServices Recovery{};
		EditorAutomationPreferenceServices AutomationPreferences{};
		EditorExternalEditorServices ExternalEditor{};
		// Memory-only copy of the recent projects (ProjectManager::ReadRecentProjects of user://Editor.json): the host reads
		// it at startup and again at the safe point after each project open or close, never while a panel draws; the error
		// of that read when it failed. Unset: no recent projects.
		std::function<Result<std::vector<std::filesystem::path>>()> GetRecentProjects{};
		// OS drops are copied as native paths; asset.import validates them and writes/provenance through existing commands.
		// External editor launch uses Process with argument lists, never a shell, and propagates Io/NotFound.
		std::function<Status(const std::filesystem::path&, uint32_t)> OpenSource{};
		// M18 (Docs/Decisions/0029-m18-contract.md decision 19): the native dialog backend every panel's FileDialog offers
		// its requests to (FileDialog::Open): MakeNativeFileDialogBackend over the main window in a windowed editor. Unset in
		// headless editors and tests: every dialog is the ImGui picker.
		FileDialogBackend FileDialogs{};
	};

}
