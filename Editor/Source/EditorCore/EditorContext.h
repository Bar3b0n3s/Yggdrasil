#pragma once

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Commands/Command.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/UniqueFunction.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The editor's state (Architecture §12.1), frozen by the M4 contract: the open project, the one open scene (v1 edits a
// single scene at a time), the selection, the undo history, write attribution and provenance, dry runs and transactions.
// Panels (M10) and automation handlers read it and change it only through Execute, the same entry point for both (§12.1),
// so every change is undoable and every agent change is labelled "[agent]".

namespace Engine {

	class AssetLoaderRegistry;
	class AudioPreview;
	class EditorAssetManager;
	class EditorDryRunScope;
	class EditorPlayController;
	class EditorTransaction;
	class EngineContext;
	class IEnvironmentBaker;
	class IMount;
	class ImporterRegistry;
	class ProjectSettingsCommand;
	class SceneEdit;
	class TypeRegistry;
	class VirtualFileSystem;
	struct AssetRefreshReport;

	struct EditorContextSpecification
	{
		// The state of the editor's UUID generator (§4.8: xoshiro256**). Absent (the editor): EditorContext::Create seeds it
		// from the OS CSPRNG (SecureRandom::GenerateState), so every session's ids are unique. Tests pass a fixed state so ids
		// are reproducible. A given state must not be all zero (asserted).
		std::optional<Random::State> IdGeneratorState{};
		// Resources/Templates/Projects (ProjectCreateSpecification::TemplatesDirectory).
		std::filesystem::path TemplatesDirectory{};
		// Where read-only editors create their private temporary cache (ProjectOpenOptions::ReadOnlyCacheDirectory); empty:
		// a directory under the system temporary directory.
		std::filesystem::path ReadOnlyCacheRoot{};
		// The undo history's bounds (§12.3: 1,000 entries or 256 MB); the 10,000-operation property test raises them.
		CommandHistoryLimits HistoryLimits{};
		// The GPU environment baker for EnvironmentImporter (§7.4, §8.6): the Renderer's EnvironmentBaker when the editor has a
		// device (M8); null with --renderer none, where environment imports reuse cooked bakes (§13.9). A documented
		// back-reference that outlives the editor.
		IEnvironmentBaker* EnvironmentBaker = nullptr;
	};

	// The editor state of one editor process (or one test). Not copyable or movable; main thread only (§4.11), like the scene.
	//
	// Write path. Every file the editor writes under the project goes through WriteProjectFile, which refuses writes in
	// read-only editors, writes into the overlay during a dry run, and otherwise writes atomically through project:// and
	// records provenance for the .eproj and Assets/ (§13.4) with the current attribution.
	//
	// Mutation path. Execute is the only way to change the open scene or the project settings: it runs a Command with the
	// current origin (Agent while an automation request is served) and records it in the history, or in the open
	// transaction, or (during a dry run) in the sandbox history that the dry run discards.
	//
	// Assets (M6). The editor owns one EditorAssetManager for its lifetime, with the built-in importers and loaders, injected
	// into the engine context (EngineContext::SetAssetManager). OpenProject opens the project's assets (scan, .meta files for
	// new sources, hot reload), CloseProject closes them; the project write path routes through the manager's AssetWriter
	// (no-echo writes, §7.5 race rule 1), so WriteProjectFile, MoveProjectFile and RemoveProjectFile update the registry and
	// schedule reimports, and the hot reloader never sees the editor's own writes. Provenance is kept for every write the
	// manager's writer reports (EditorAssetManager::SetWriteObserver), so the .meta files the manager writes itself (new
	// sources, dependency metas, sub-asset updates) are recorded with the current attribution like the editor's own writes
	// (§13.12 rule 1). Update drives hot reload once per frame. An external change of the open scene's file, or of a prefab
	// it instantiates, raises SceneChangedOnDisk (§7.5 race rule 3) and is never reloaded silently.
	//
	// Prefab updates (§5.5 "Update": instances are rebuilt as prefab + overrides inside one undoable command). Every editor
	// action that gives a Prefab asset the open scene instantiates a new version composes its asset command with
	// CreatePrefabUpdateCommand into one undo step: prefab.apply, and asset.setImportSettings and asset.reimport of such an
	// asset (a glTF), whose import then runs before the method returns. External changes are never applied silently: they
	// raise SceneChangedOnDisk, and scene.open (reload included) rebuilds the loaded scene's instances from the current
	// prefab versions (UpdatePrefabInstances), so adopting the disk version adopts the changed prefabs too.
	class EditorContext
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class EditorContext;
		};

		// Use Create, which resolves the id generator's state.
		EditorContext(ConstructionKey key, EngineContext& engine, const EditorContextSpecification& specification,
			const Random::State& idGeneratorState);
		~EditorContext();

		EditorContext(const EditorContext&) = delete;
		EditorContext& operator=(const EditorContext&) = delete;

		// An editor without a project (the launcher state, §12.1) over `engine` (a documented back-reference that outlives
		// the editor; its TypeRegistry holds the editor's automation types). Errors: those of SecureRandom::GenerateState (Io)
		// when the specification gives no IdGeneratorState.
		[[nodiscard]] static Result<Scope<EditorContext>> Create(EngineContext& engine, const EditorContextSpecification& specification);

		// Services, from the engine context.
		[[nodiscard]] EngineContext& GetEngine() const { return *m_Engine; }
		[[nodiscard]] const TypeRegistry& GetTypeRegistry() const;
		[[nodiscard]] VirtualFileSystem& GetVfs() const;
		// The generator of the editor's new entity ids (also handed to repair loads).
		[[nodiscard]] UUIDGenerator& GetIdGenerator() { return m_IdGenerator; }
		// The specification as given (Create fills an absent IdGeneratorState only into the generator, not into this copy).
		[[nodiscard]] const EditorContextSpecification& GetSpecification() const { return m_Specification; }
		// The editor's asset manager (see the class comment); valid for the editor's lifetime.
		[[nodiscard]] EditorAssetManager& GetAssets() { return *m_Assets; }
		[[nodiscard]] const EditorAssetManager& GetAssets() const { return *m_Assets; }
		// M12: the asset browser's audio preview (EditorCore/Audio/AudioPreview.h) over the engine context's AudioEngine;
		// nullptr when the engine context has none (most in-process tests). Valid for the editor's lifetime; CloseProject
		// stops it.
		[[nodiscard]] AudioPreview* GetAudioPreview() { return m_AudioPreview.get(); }

		// Once per frame (EditorApp::OnUpdate) with a monotonic time in seconds (the frame clock's accumulated unscaled time,
		// so headless editors on a ManualClock are deterministic); tests pass chosen times: drives asset hot reload
		// (EditorAssetManager::Update; nothing without a project) and the audio preview (AudioPreview::Update, M12).
		void Update(double nowSeconds);

		// --- Project -------------------------------------------------------------------------------------------------------

		[[nodiscard]] bool HasProject() const { return m_Project != nullptr; }
		// The open project (asserted HasProject).
		[[nodiscard]] const LoadedProject& GetProject() const;
		// The open project was opened with --read-only; false without a project.
		[[nodiscard]] bool IsReadOnly() const;

		// Takes the opened project (ProjectManager::OpenProject): mounts project:// at its root (keeping .bak files, read-only
		// for a read-only project) and cache:// at its cache directory, loads its provenance (none for read-only projects),
		// adds it to the recent list (when user:// is mounted; a failure there is logged, not returned), opens its assets
		// (EditorAssetManager::OpenProject on project://Assets, which is created first when a writable project lacks it) and
		// starts with no open scene and an empty history. Errors: InvalidState when a project is already open; the mount
		// errors, ProvenanceRecorder::Load errors, the errors of creating Assets/ and EditorAssetManager::OpenProject errors,
		// after which the project stays closed.
		[[nodiscard]] Status OpenProject(Scope<LoadedProject> project);

		// Closes the open scene and the project (unmounts project:// and cache://, releases the lock). Unsaved changes are
		// discarded: callers ask IsSceneDirty first. No effect without a project.
		void CloseProject();

		// --- The open scene ------------------------------------------------------------------------------------------------

		[[nodiscard]] bool HasScene() const { return m_Scene != nullptr; }
		// The open scene (asserted HasScene).
		[[nodiscard]] Scene& GetScene();
		[[nodiscard]] const Scene& GetScene() const;
		// The file the open scene was opened from or last saved to; nullopt for a scene that was never saved.
		[[nodiscard]] const std::optional<VfsPath>& GetScenePath() const { return m_ScenePath; }

		// A new, empty edit scene named `name` on the context's registry and id generator; not yet open.
		[[nodiscard]] Scope<Scene> CreateScene(std::string name);

		// Makes `scene` (created with CreateScene, or loaded into one) the open scene, from `path`: clears the history and the
		// selection, sets the dirty flag to `dirty` (a repaired load is dirty until saved), appends a SceneOpened event and
		// logs the scene's name and entity count. Asserts a project, and a scene on this context's registry and generator.
		void SetScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty = false);

		// Closes the open scene without saving; no effect without one.
		void CloseScene();

		// True when the open scene has unsaved changes: a scene-changing command since the save point, or a dirty SetScene
		// (repairs) not saved since.
		[[nodiscard]] bool IsSceneDirty() const;

		// The open scene was written to `path` (scene.save, project.save): sets the scene path and the save point, and clears
		// SceneChangedOnDisk.
		void MarkSceneSaved(const VfsPath& path);

		// §7.5 race rule 3: true once the open scene's file, or a prefab asset the open scene instantiates (a
		// PrefabInstanceComponent names it), changed on disk through something other than this editor since the scene was
		// opened, saved or reloaded. The change appended a SceneChangedOnDisk event {Path, Dirty} (§4.9) and "_meta" reports
		// sceneChangedOnDisk: true (MetaState::SceneChangedOnDisk); the in-memory scene is left alone until scene.open {path,
		// reload: true} (plus discardChanges when dirty) adopts the disk version and the current versions of its prefabs.
		// SetScene and MarkSceneSaved clear it. False without an open scene. Raised once per change, whether a poll or a
		// Refresh (project.refreshAssets, or the implicit refresh of a path-taking automation call) detected it.
		[[nodiscard]] bool IsSceneChangedOnDisk() const { return m_SceneChangedOnDisk; }

		// §5.5 "Update": rebuilds, in `scene` (the open scene or one scene.open just loaded), every instance of the Prefab
		// assets `prefabs` (sorted; every instance in the scene when empty) from the prefab's current version through
		// PrefabInstantiator (the single path, §5.5), keeping each instance's root ID, overrides and user children. Returns
		// whether anything changed. Not a command: scene.open (reload included) calls it on the scene it loaded before
		// SetScene, and the scene opens dirty when it changed something (like a repair). An instance whose prefab is missing
		// is left as it is (PREFAB_MISSING_ASSET, ProjectValidator). Errors: those of PrefabInstantiator and of loading the
		// prefab assets (LoadPrefabAsset).
		[[nodiscard]] Result<bool> UpdatePrefabInstances(Scene& scene, std::span<const AssetHandle> prefabs);

		// The undoable form for the open scene: a SceneEditCommand labelled "Update Prefab Instances" whose effect is
		// UpdatePrefabInstances(GetScene(), prefabs), or nullptr when it would change nothing. The methods that give a
		// prefab a new version compose it with their asset command (see the class comment). Errors: InvalidState without an
		// open scene; those of UpdatePrefabInstances.
		[[nodiscard]] Result<Scope<Command>> CreatePrefabUpdateCommand(std::span<const AssetHandle> prefabs);

		// The editor's revision (§13.4: "_meta".revision, ifRevision, the revisions in results and history entries, scene.diff
		// {against}). It increases with every mutation of the open scene and never repeats a value for the editor's lifetime,
		// across scene.open, scene.new, reloads and CloseScene, so an ifRevision taken in one scene can never match another
		// (no ABA): it is the open scene's own revision (Scene::GetRevision, which restarts with every Scene object) plus a
		// base that SetScene and CloseScene advance past every value reported so far. 0 before the first scene opens. During
		// a dry run it is the real revision plus the mutations the sandbox copy has seen, so a dry run reports what the real
		// call would produce (EditorDryRunScope adjusts the base while it swaps the scene).
		[[nodiscard]] uint64_t GetRevision() const;

		// --- Commands (the single mutation entry point, §12.1) -----------------------------------------------------------

		// Sets `command`'s origin from GetCommandOrigin, executes it and records it: in the open transaction when there is
		// one, else in the history (in the dry run's sandbox history during a dry run). Returns the undo index (CommandHistory
		// sequence; 0 inside a transaction or a dry run, where the index is only known at Commit). Errors: PermissionDenied for
		// a read-only project (outside dry runs); the command's own error; nothing changed then.
		[[nodiscard]] Result<uint64_t> Execute(Scope<Command> command);

		// The history of the open scene (the sandbox history during a dry run).
		[[nodiscard]] CommandHistory& GetHistory();
		[[nodiscard]] const CommandHistory& GetHistory() const;

		// The open transaction, or nullptr.
		[[nodiscard]] EditorTransaction* GetTransaction() const { return m_Transaction; }

		// --- Selection (§12.1 SelectionSet; edit.select, edit.getSelection) ---------------------------------------------

		// The selected entities, in the order selected, without duplicates.
		[[nodiscard]] std::span<const UUID> GetSelection() const { return m_Selection; }
		// Replaces the selection; ids that are not entities of the open scene are dropped, duplicates removed (first kept).
		// Selection is editor state, not a command (it is never undone).
		void SetSelection(std::vector<UUID> selection);

		// --- Write attribution and provenance (§13.4) ----------------------------------------------------------------------

		// Writes `data` to `path` (a project:// path, asserted) atomically and, for the .eproj and Assets/, records provenance
		// with the current attribution and saves Automation/Provenance.json. During a dry run the bytes go to the overlay and
		// nothing is recorded. Errors: PermissionDenied only for a read-only project (the protocol reports it as
		// Unauthorized); the VFS write errors (Validation for a case mismatch, Io), with an operating-system access failure
		// (PermissionDenied from the file system: EACCES, EPERM, a sharing violation) converted to Io naming the OS error
		// (JsonRpc.h ToRpcErrorCode); nothing recorded then.
		[[nodiscard]] Status WriteProjectFile(const VfsPath& path, std::span<const std::byte> data);

		// Renames a file or directory under the project (project:// paths, asserted; both in one scheme) through the asset
		// writer, creating the destination's parent directories. Provenance moves with it (through the write observer): a
		// recorded path's entry is removed and, when the destination is recorded too (under Assets/), re-recorded with the
		// file's hash and the current attribution (a move into Library/Trash leaves no entry, §13.12 rule 3). During a dry run
		// the move happens in the overlay and nothing is recorded. Errors: PermissionDenied for a read-only project; the VFS errors (NotFound,
		// AlreadyExists, Validation for a case mismatch, Io with OS access failures converted like WriteProjectFile's).
		[[nodiscard]] Status MoveProjectFile(const VfsPath& from, const VfsPath& to);

		// Removes a file or an empty directory under the project through the asset writer, and its provenance entry (undo of a
		// creation; the user-facing delete moves to the trash instead, AssetDeleteCommand). During a dry run the removal
		// happens in the overlay and nothing is recorded. Errors: as MoveProjectFile; InvalidState for a non-empty directory.
		[[nodiscard]] Status RemoveProjectFile(const VfsPath& path);

		// Creates a directory (and missing parents) under the project through the asset writer (asset.create {type: Folder},
		// moves into new folders). Errors: as MoveProjectFile.
		[[nodiscard]] Status CreateProjectDirectory(const VfsPath& directory);

		// The attribution of the request being served (set by AutomationServer::EnterInvocation, cleared by LeaveInvocation),
		// or nullopt for UI writes (Method "ui").
		void SetWriteAttribution(std::optional<WriteAttribution> attribution);
		[[nodiscard]] const WriteAttribution& GetWriteAttribution() const { return m_Attribution; }
		// Agent while an automation request is served (an attribution is set), User otherwise.
		[[nodiscard]] CommandOrigin GetCommandOrigin() const { return m_HasRequestAttribution ? CommandOrigin::Agent : CommandOrigin::User; }

		// The provenance of the open writable project; nullptr without a project and for read-only projects.
		[[nodiscard]] const ProvenanceRecorder* GetProvenance() const;

		// --- Events -----------------------------------------------------------------------------------------------------

		// Appends `event` to the engine's EventLog (§4.9), except during a dry run, which leaves no trace (§13.4).
		void AppendEvent(EngineEvent event);

		// --- Dry runs (§13.4) -------------------------------------------------------------------------------------------

		// True while an EditorDryRunScope is open.
		[[nodiscard]] bool IsDryRun() const { return m_DryRun != nullptr; }
		// The open dry run, or nullptr.
		[[nodiscard]] EditorDryRunScope* GetDryRun() const { return m_DryRun; }

		// --- Play mode (§5.6, §12.4; M7) ---------------------------------------------------------------------------------

		// The editor's play mode: at most one PlaySession, made from the open edit scene (EditorPlayController). Valid for the
		// editor's lifetime; a running session is stopped before the project closes.
		[[nodiscard]] EditorPlayController& GetPlay() { return *m_Play; }
		[[nodiscard]] const EditorPlayController& GetPlay() const { return *m_Play; }

		// The camera of the editor's scene view (§8.13; viewport.screenshot {view: "scene"}): the default
		// ExplicitRenderCamera until the editor camera and viewport.camera arrive with the editor panels (M10;
		// Docs/Decisions/0012-m7-decisions.md decision 9).
		[[nodiscard]] const ExplicitRenderCamera& GetSceneViewCamera() const { return m_SceneViewCamera; }

		// --- Shutdown (session.shutdown) --------------------------------------------------------------------------------

		// Asks the application to exit with `exitCode` after the current frame; the first request wins. EditorApp polls it at
		// its safe point.
		void RequestShutdown(int exitCode);
		[[nodiscard]] std::optional<int> GetShutdownRequest() const { return m_ShutdownRequest; }
	private:
		// Replaces the project settings with the canonical ProjectSerializer document `document` and writes it to the .eproj
		// through WriteProjectFile (§12.3). Private: settings change only through ProjectSettingsCommand, executed by Execute
		// (the class rule above). Errors: InvalidState without a project; Validation for an invalid document; the write
		// errors; nothing changed then.
		[[nodiscard]] Status ApplyProjectSettings(const Json& document);

		// The revision a command about to be recorded started from: the revision before a SceneEdit's mutations while its
		// Commit executes the command (SceneEditCommand is built applied), else GetRevision. CommandHistory and
		// EditorTransaction record it as the entry's RevisionBefore (§12.3).
		[[nodiscard]] uint64_t GetRevisionBeforeCommand() const;
		// A mount of the open project's root as OpenProject mounts it at project:// (read-only for a read-only project,
		// keeping .bak files). Errors: those of NativeDirectoryMount::Create.
		[[nodiscard]] Result<Scope<IMount>> CreateProjectMount() const;
		// The checks every write path member makes before it changes `path` ("cannot <action> '<path>'"): InvalidState without
		// a project, PermissionDenied for a read-only project outside dry runs. Asserts a project:// path.
		[[nodiscard]] Status CheckProjectWrite(const VfsPath& path, std::string_view action) const;
		// EditorAssetManager::OpenProject on the open project's project://Assets (created first when a writable project lacks
		// it) and cache://, read-only for a read-only project, with hot reload. Errors: those of the creation and of OpenProject.
		[[nodiscard]] Result<AssetRefreshReport> OpenProjectAssets();
	private:
		EngineContext* m_Engine = nullptr; // documented back-reference: outlives the editor
		EditorContextSpecification m_Specification;
		UUIDGenerator m_IdGenerator;
		// The asset services (M6), after the generator the manager draws handles from, so they are destroyed before it.
		Scope<ImporterRegistry> m_Importers;
		Scope<AssetLoaderRegistry> m_Loaders;
		Scope<EditorAssetManager> m_Assets;
		Scope<AudioPreview> m_AudioPreview; // M12: after the manager it loads clips through, so it is destroyed first
		bool m_SceneChangedOnDisk = false;  // IsSceneChangedOnDisk
		Scope<LoadedProject> m_Project;
		Scope<Scene> m_Scene;
		std::optional<VfsPath> m_ScenePath;
		bool m_SceneDirty = false;   // a dirty SetScene (a repaired load) not saved since; the history tracks the rest
		uint64_t m_RevisionBase = 0; // GetRevision's base, advanced by SetScene and CloseScene
		CommandHistory m_History;    // the open scene's history; a dry run swaps its sandbox history in
		std::vector<UUID> m_Selection;
		WriteAttribution m_Attribution;
		bool m_HasRequestAttribution = false;
		std::optional<ProvenanceRecorder> m_Provenance;  // the open writable project's provenance
		std::optional<uint64_t> m_PendingRevisionBefore; // GetRevisionBeforeCommand's value while SceneEdit::Commit executes
		EditorTransaction* m_Transaction = nullptr;      // the outermost open transaction, which registers itself
		EditorDryRunScope* m_DryRun = nullptr;           // the open dry run, which registers itself
		std::optional<int> m_ShutdownRequest;
		// While a write path member that can change provenance (WriteProjectFile, MoveProjectFile, RemoveProjectFile) runs, the
		// write observer keeps the first provenance save failure here for that member to return; otherwise it logs it.
		bool m_CollectProvenanceErrors = false;
		std::optional<Error> m_ProvenanceError;
		ExplicitRenderCamera m_SceneViewCamera{};
		// The undo of each play-scene change a SceneEdit committed inside the open dry run or transaction (M7, §13.4: play-scene
		// edits are transient and never recorded, yet a dry run leaves no trace and a failed edit.batch rolls back every op):
		// run newest first when the dry run ends or the transaction rolls back, dropped when the outermost transaction commits
		// outside a dry run. SceneEdit::Commit appends them.
		std::vector<UniqueFunction<void()>> m_TransientPlayUndos;
		// Last, so it is destroyed first: a play session refers to the asset manager and the type registry (M7).
		Scope<EditorPlayController> m_Play;
	private:
		friend class CommandHistory; // reads GetRevisionBeforeCommand
		friend class EditorDryRunScope;
		friend class EditorTransaction;
		friend class ProjectSettingsCommand; // the one caller of ApplyProjectSettings
		friend class SceneEdit;              // sets m_PendingRevisionBefore around its Execute and appends m_TransientPlayUndos
	};

	// Groups every command executed while it is open into one CompositeCommand, recorded as one undo step (§12.3:
	// transactions, edit.batch, validator fixes). Main thread; scoped (opened and closed in stack order). Not copyable or
	// movable.
	//
	// While it is open, EditorContext::Execute runs each command immediately and appends it to the composite. Commit records
	// the composite in the history (a dry run's sandbox history during a dry run) as one entry; destroying an uncommitted
	// transaction rolls back: the executed commands are undone in reverse order, so a failed op k of edit.batch leaves the
	// scene exactly as before ops 0 .. k-1 (§13.4 "Atomic batch").
	//
	// Nesting joins. A transaction opened while another is open on the same context (ProjectValidator::Fix run by an op
	// project.validate {fix} of edit.batch) joins the outermost one instead of asserting: the commands it executes are
	// appended to the outermost transaction, its Commit records nothing and returns 0 (the outermost Commit records them all
	// as one step), and its Rollback undoes only the commands it executed, newest first, and removes them from the
	// outermost transaction.
	class EditorTransaction
	{
	public:
		// Opens a transaction labelled `label` (the undo label without origin prefix) on `context`, or joins the open one.
		EditorTransaction(EditorContext& context, std::string label);
		// Rollback when still open; a rollback failure there is logged at Error (Rollback's error), never asserted.
		~EditorTransaction();

		EditorTransaction(const EditorTransaction&) = delete;
		EditorTransaction& operator=(const EditorTransaction&) = delete;
		EditorTransaction(EditorTransaction&&) = delete;
		EditorTransaction& operator=(EditorTransaction&&) = delete;

		// Closes the transaction and records its commands as one CompositeCommand. Returns its undo index (0 when no command
		// was executed, in which case nothing is recorded, 0 during a dry run, and 0 for a joined transaction).
		[[nodiscard]] uint64_t Commit();

		// Closes the transaction and undoes the commands it executed, newest first. Command::Undo can fail (a settings
		// command's file write); the rollback then stops at the failing command, and the commands still applied (that one
		// and the older ones) are recorded as one CompositeCommand labelled "<label> (partially rolled back)" (in the
		// outermost transaction when joined, else in the history), so the scene still matches an undo step and nothing is
		// lost. Errors: that Undo's error with the context "rolling back '<label>'". Idempotent; nothing after Commit.
		[[nodiscard]] Status Rollback();

		// EditorContext::Execute's path while the transaction is open: executes `command` and appends it. Errors: the
		// command's own; the transaction stays open and its earlier commands stay executed (the caller decides to roll back).
		[[nodiscard]] Status ExecuteAndAppend(Scope<Command> command);

		[[nodiscard]] bool IsOpen() const { return m_IsOpen; }
		// True when this transaction joined an outer one.
		[[nodiscard]] bool IsJoined() const;
		// The number of commands this transaction executed so far.
		[[nodiscard]] size_t GetCommandCount() const;
	private:
		// The context back-reference, the label, the outermost transaction when joined, the executed commands (or, when
		// joined, how many of the outermost's commands are this one's) and the revision before the first command
		// (EditorContext.cpp).
		struct State;
	private:
		Scope<State> m_State;
		bool m_IsOpen = true;
	};

	// The sandbox of one dry run (§13.4: "Scene mutations execute then undo inside a sandboxed history. File-writing methods
	// write into an OverlayMount layered over project:// for the duration of the call"). Not copyable or movable; main thread.
	//
	// Begin waits until no job uses project:// (JobSystem::WaitIdle, ADR 0003 decision 13) and then, until the scope ends:
	//   - project:// is unmounted, wrapped in an OverlayMount and mounted again, so writes, parsing and registry updates see
	//     the would-be files while nothing reaches the disk, the file watcher or the trash;
	//   - the open scene is replaced by a serializer copy of itself (§1.3, the one scene-copy path) with a sandbox history,
	//     and the id generator by a copy of itself, so the real scene's revision, history and selection are untouched and the
	//     ids a dry run reports are the ids the real call would create next;
	//   - project settings changes stay in memory and in the overlay;
	//   - provenance and the event log record nothing.
	// The destructor discards all of it and restores the real scene, history, generator, settings and mount (ADR 0008
	// decision 12: a copy instead of execute-then-undo, so the real scene's revision stays unchanged).
	class EditorDryRunScope
	{
	public:
		// Restricts construction to Begin; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class EditorDryRunScope;
		};

		// Use Begin. Records `context` only; Begin then swaps the sandbox in.
		EditorDryRunScope(ConstructionKey key, EditorContext& context);

		// Opens a dry run on `context` (asserted: none open, no transaction open). Errors: those of unmounting and mounting
		// project:// (with everything restored), and Validation when the open scene cannot be serialized.
		[[nodiscard]] static Result<Scope<EditorDryRunScope>> Begin(EditorContext& context);

		~EditorDryRunScope();

		EditorDryRunScope(const EditorDryRunScope&) = delete;
		EditorDryRunScope& operator=(const EditorDryRunScope&) = delete;

		// The project-relative paths the dry run wrote, created, removed or moved (OverlayMount::GetChangedPaths), sorted:
		// what the real call would change on disk (project.upgrade's dry run reports them).
		[[nodiscard]] std::vector<std::string> GetChangedPaths() const;
	private:
		// The context back-reference and what the dry run swapped out: the real scene, history, id generator, settings,
		// revision base and project:// mount, and the OverlayMount (EditorContext.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
