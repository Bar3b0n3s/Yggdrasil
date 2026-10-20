#pragma once

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Commands/Command.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorUiState.h"
#include "EditorCore/OpenSceneId.h"
#include "EditorCore/Project/ProjectManager.h"
#include "EditorCore/Viewport/EditorViewportState.h"
#include "Engine/Asset/AssetHandle.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
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
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The editor's state (Architecture §12.1), frozen by the M4 contract: the open project, the open scenes (one before M20; M20
// keeps several open, one of them active: Docs/Decisions/0031-m20-contract.md decision 13), the selection, the undo history,
// write attribution and provenance, dry runs and transactions. Panels (M10) and automation handlers read it and change it
// only through Execute, the same entry point for both (§12.1), so every change is undoable and every agent change is
// labelled "[agent]".

namespace Engine {

	class AssetLoaderRegistry;
	class AudioPreview;
	class EditorAssetManager;
	class EditorDryRunScope;
	class EditorPlayController;
	class EditorScriptService;
	class ScriptTypeChecker;
	class ScriptFieldSchemaSource;
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
	struct AssetWriteContext;

	// One open scene as EditorContext::DescribeOpenScene reports it (scene.list's entries, decision 14).
	struct OpenSceneInfo
	{
		OpenSceneId Id = NoOpenScene;
		std::optional<VfsPath> Path{}; // the file it was opened from or last saved to; nullopt for a scene never saved
		std::string Name{};
		uint64_t Revision = 0; // its EditorContext::GetRevision
		size_t EntityCount = 0;
		bool Active = false;        // the active scene (EditorContext::GetActiveSceneId)
		bool Dirty = false;         // its EditorContext::IsSceneDirty
		bool ChangedOnDisk = false; // its EditorContext::IsSceneChangedOnDisk
	};

	// Main-thread host hooks. Captured services must outlive the binding; clear it only after asynchronous writers
	// and the fatal hook are quiescent, before destroying those services. Empty callbacks preserve CPU-only behavior.
	struct EditorLifecycleCallbacks
	{
		std::function<Status()> BeforePlay{};
		std::function<Status()> AfterSceneSaved{};
		std::function<Status()> BeforeProjectClose{};
	};

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
		// --strict (§6 "Strict reader"; the M15 completion review's A024): the projects this editor opens are read strictly.
		// Every opener (EditorApp's --project, project.open, project.create, recovery) passes it as
		// ProjectOpenOptions::StrictUnknowns, so unknown keys in the .eproj fail the open; OpenProject then loads every scene
		// and prefab under Assets/ strictly (LoadMode::Strict with LoadOptions::StrictUnknowns and the script field schemas)
		// and fails, leaving the project closed, naming every file that does not load: one with an unknown key, field or
		// component, and equally one with a structural defect, one that cannot be read or parsed, or one whose Version is
		// newer than this build reads, since CI wants every committed scene and prefab to load strictly (the M15 fix review's
		// C2); and the editor's own loads of scene files (scene.open and the methods that reopen a scene file, the exporter's
		// scene loads) are strict too. Off: unknown keys, fields and components are warnings, and the scenes and prefabs are
		// not loaded when the project opens.
		bool StrictUnknowns = false;
		// --upgrade (Docs/Decisions/0026-m16-contract.md decision 11): OpenProject mounts project:// and cache://, loads the
		// provenance and stops before the asset scan, so opening the project writes nothing. The script services, creating
		// Assets/, EditorAssetManager::OpenProject (the scan and the .meta files it writes for new sources), the --strict
		// document check and the recent-list entry wait for OpenDeferredAssets, which project.refreshAssets, the first request
		// of --upgrade's cli batch [project.refreshAssets, project.upgrade], runs under its own attribution and transcript line
		// (§13.12 rule 2). Until then the asset manager has no project, and nothing but that request is served. Off (the
		// default): OpenProject opens the assets at once.
		bool DeferAssetScan = false;
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
	//
	// Open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decisions 13 to 16). The editor holds up to
	// MaxOpenScenes open scenes in tab order, one of them active: the tab the editor shows, play mode plays and the UI edits.
	// Each open scene is its own document, with its own scene, path, dirty and SceneChangedOnDisk flags, undo history,
	// revision, document epoch and selection. The members that name "the open scene" (HasScene, GetScene, GetScenePath,
	// IsSceneDirty, MarkSceneSaved, IsSceneChangedOnDisk, UpdatePrefabInstances' command form, GetRevision,
	// GetSceneDocumentEpoch, GetHistory, Execute's history, CloseScene() and the selection) address one of them: the
	// addressed scene, which is the active scene unless an EditorSceneTargetScope is open (a request with a scene target), and
	// that scope's scene while one is. So GetScene() stays the active-scene accessor for every caller outside a targeted
	// request, and a targeted request's handlers and commands address its scene without passing it on. A command that changes
	// no scene (project settings, an asset edit) is recorded in the addressed scene's history, as it is recorded in the one
	// history before M20; with no open scene it goes to the editor's scene-less history, which the next scene opened
	// replaces. Revisions never repeat across the open scenes (GetRevision). An editor write of a Prefab asset (any command,
	// its undo and its redo) rebuilds that prefab's instances in every other open scene that instantiates it, as one "Update
	// Prefab Instances" entry in that scene's own history, so a prefab update reaches every open scene while undo stays
	// isolated per scene (decision 15).
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
		// M13: borrowed main-thread service, null without an open project and while its asset scan is deferred
		// (IsAssetScanDeferred). The context owns its diagnostics provider
		// and shares that provider with imports. Before replacing configuration, drain jobs, then recreate both borrowed
		// references; the provider outlives every job and service. The pointer expires at project/configuration changes.
		[[nodiscard]] EditorScriptService* GetScriptService();
		// Refreshes the checker configuration and preserves infrastructure errors for operations that require checking.
		// Errors: InvalidState without a project or while its asset scan is deferred; the configuration errors.
		[[nodiscard]] Result<EditorScriptService*> AcquireScriptService();
		// Observes the service initialized by OpenProject; never creates or replaces state through a const editor.
		// Configuration snapshots are refreshed by Update and the mutable accessor before mutable host operations.
		[[nodiscard]] const EditorScriptService* GetScriptService() const;
		// Pinned current cooked schemas; caller retains this snapshot through reflection/UI work. Failed imports
		// preserve their last good artifact; unknown scripts remain unresolved instead of inventing descriptors.
		[[nodiscard]] Result<Ref<const ScriptFieldSchemaSource>> GetScriptSchemaSnapshot() const;
		// M12: the asset browser's audio preview (EditorCore/Audio/AudioPreview.h) over the engine context's AudioEngine;
		// nullptr when the engine context has none (most in-process tests). Valid for the editor's lifetime; CloseProject
		// stops it.
		[[nodiscard]] AudioPreview* GetAudioPreview() { return m_AudioPreview.get(); }

		// Once per frame (EditorApp::OnUpdate) with a monotonic time in seconds (the frame clock's accumulated unscaled time,
		// so headless editors on a ManualClock are deterministic); tests pass chosen times: drives asset hot reload
		// (EditorAssetManager::Update; nothing without a project, nor while the asset scan is deferred, when the asset manager
		// has no project) and the audio preview (AudioPreview::Update, M12).
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
		// starts with no open scene and an empty history. A --strict editor (EditorContextSpecification::StrictUnknowns) then
		// loads every scene and prefab under Assets/ strictly. Errors: InvalidState when a project is already open; the mount
		// errors, ProvenanceRecorder::Load errors, the script services' errors (Validation for a .luaurc the type checker
		// cannot read), the errors of creating Assets/ and EditorAssetManager::OpenProject errors; with StrictUnknowns, a
		// Validation error naming every scene and prefab that does not load strictly (one issue per file: its path, its first
		// problem and pointer); after any of them the project stays closed.
		// With EditorContextSpecification::DeferAssetScan (Docs/Decisions/0026-m16-contract.md decision 11) it stops after
		// loading the provenance, with no open scene and an empty history, and IsAssetScanDeferred() is then true: the script
		// services, creating Assets/, the asset scan, the strict check and the recent-list entry wait for OpenDeferredAssets,
		// whose errors theirs become, and OpenProject's own errors are InvalidState, the mount errors and
		// ProvenanceRecorder::Load errors.
		[[nodiscard]] Status OpenProject(Scope<LoadedProject> project);

		// True from an OpenProject that deferred the asset scan (EditorContextSpecification::DeferAssetScan) until
		// OpenDeferredAssets ran it successfully or the project closed; false without a project
		// (Docs/Decisions/0026-m16-contract.md decision 11).
		[[nodiscard]] bool IsAssetScanDeferred() const;

		// Runs the part of OpenProject that DeferAssetScan held back, with the current attribution: the script services,
		// creating Assets/, EditorAssetManager::OpenProject on project://Assets (the scan, and the .meta files it writes for
		// new sources, which provenance records with that attribution), for a --strict editor the strict document check, and
		// then the recent-list entry. project.refreshAssets calls it before its refresh while the scan is deferred and reports
		// both, so every write of --upgrade's open is attributed to that cli request and its transcript line (§13.12 rule 2).
		// Returns the scan's report. Errors: InvalidState without a project or when no scan is deferred; else the errors
		// OpenProject documents for that part. A failure undoes only that part (the asset manager's project and the script
		// services) and leaves the project open, with project:// and cache:// mounted and IsAssetScanDeferred() still true, so
		// the failing request's response line is still transcribed and its error is reported as its own; the caller closes the
		// project (EditorApp exits non-zero after the failed batch and closes it as usual).
		[[nodiscard]] Result<AssetRefreshReport> OpenDeferredAssets();

		// Closes the open scene and the project (unmounts project:// and cache://, releases the lock). Unsaved changes are
		// discarded: callers ask IsSceneDirty first. No effect without a project. BeforeProjectClose runs before stopping
		// play or changing state/mounts; its failure leaves the project and lock intact for a later retry.
		[[nodiscard]] Status CloseProject();

		void SetLifecycleCallbacks(EditorLifecycleCallbacks callbacks);
		// Called by EditorPlayController after validating options and before serializing the runtime scene copy.
		// A failure prevents play. Disposable dry runs do not call host services.
		[[nodiscard]] Status PrepareForPlay();

		// --- The open scene (the addressed one, see the class comment) -----------------------------------------------------

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
		// M20 (decision 13): `scene` replaces the addressed scene in its tab position (opens the first scene when none is
		// open), gets a new OpenSceneId and becomes the active scene; the other open scenes are untouched.
		void SetScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty = false);

		// Closes the open scene without saving; no effect without one. M20: the addressed scene, as CloseScene(id) closes it.
		void CloseScene();

		// --- Open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decisions 13 and 14) ------------------------

		// The open scenes in tab order: the order they were opened, a scene replaced by SetScene keeping its place. Empty
		// without an open scene.
		[[nodiscard]] std::vector<OpenSceneId> GetOpenScenes() const;
		// The active scene; NoOpenScene without an open scene.
		[[nodiscard]] OpenSceneId GetActiveSceneId() const;
		// The addressed scene (the class comment): the open EditorSceneTargetScope's scene, else the active one.
		[[nodiscard]] OpenSceneId GetAddressedSceneId() const;
		// The open scene whose path is `path`; NoOpenScene when none is.
		[[nodiscard]] OpenSceneId FindOpenScene(const VfsPath& path) const;
		// The scene of the open scene `id`, or nullptr when no open scene has that id. Valid until it closes or is replaced.
		[[nodiscard]] Scene* FindScene(OpenSceneId id);
		[[nodiscard]] const Scene* FindScene(OpenSceneId id) const;
		// What scene.list reports of the open scene `id`. Errors: NotFound when no open scene has that id.
		[[nodiscard]] Result<OpenSceneInfo> DescribeOpenScene(OpenSceneId id) const;
		// True when any open scene has unsaved changes (the close and shutdown prompts, project.save, project.upgrade).
		[[nodiscard]] bool IsAnySceneDirty() const;

		// scene.open and scene.new with keepOpen (decision 14): opens `scene` (created with CreateScene, or loaded into one)
		// from `path` as an additional open scene at the end of the tab order and makes it active, with its own empty history
		// and selection, the dirty flag `dirty`, and the SceneOpened event and log line of SetScene. Returns its id. Errors:
		// InvalidState when MaxOpenScenes scenes are open, inside a transaction or dry run, or while a test run owns the
		// editor; AlreadyExists when an open scene already has `path`. Asserts a project and a scene on this context's
		// registry and generator.
		[[nodiscard]] Result<OpenSceneId> AddScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty = false);

		// scene.activate: makes the open scene `id` the active one, appending a SceneActivated event; every open scene keeps
		// its document (history, selection, dirty flag). No effect when it is already active. Allowed while playing: the
		// session keeps playing the scene it started from. Inside an EditorSceneTargetScope only the scope's own scene can be
		// activated (scene.activate's target is its subject). Errors: NotFound when no open scene has that id; InvalidState
		// inside a transaction, a dry run or a scope of another scene, or while a test run owns the editor.
		[[nodiscard]] Status ActivateScene(OpenSceneId id);

		// scene.close: closes the open scene `id` without saving (callers ask its dirty flag first) and discards its history,
		// appending a SceneClosed event. When it was active, the next scene in tab order becomes active (the previous one when
		// it was the last), and no scene is open after the only one closes. Allowed while playing, the played scene's tab
		// included: the session plays on, as it does when scene.open replaces the played scene, and then belongs to no open
		// scene (decision 13). Inside an EditorSceneTargetScope only the scope's own scene can be closed (scene.close's
		// target is its subject); the scope then addresses no scene until it ends. Errors: NotFound when no open scene has
		// that id; InvalidState inside a transaction, a dry run or a scope of another scene, or while a test run owns the
		// editor.
		[[nodiscard]] Status CloseScene(OpenSceneId id);

		// True when the open scene has unsaved changes: a scene-changing command since the save point, or a dirty SetScene
		// (repairs) not saved since.
		[[nodiscard]] bool IsSceneDirty() const;

		// The open scene was written to `path` (scene.save, project.save): sets the scene path and the save point, and clears
		// SceneChangedOnDisk, then calls AfterSceneSaved outside a dry run. Callback failure is returned with a hint
		// explaining that the scene was saved but recovery cleanup failed; it never makes the durable save dirty again.
		[[nodiscard]] Status MarkSceneSaved(const VfsPath& path);

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
		//
		// Live nested prefabs (M20, Docs/Decisions/0031-m20-contract.md decision 8): an instance is rebuilt only from its
		// outermost root (PrefabInstantiator), so both functions rebuild every outermost instance whose prefab is one of
		// `prefabs` or nests one of them, directly or through others; a nested instance follows its outermost one.
		// Open scenes (M20, decision 15): the command updates the addressed scene, in the caller's undo step; the prefab
		// asset write it is composed with rebuilds the other open scenes' instances, each as an entry of its own history.
		[[nodiscard]] Result<Scope<Command>> CreatePrefabUpdateCommand(std::span<const AssetHandle> prefabs);

		// Live nested prefabs (M20, decision 8). The Prefab assets that nest one of `prefabs`, directly or through others
		// (read from the registry's import dependencies, which a prefab's nested instances join), in the order they must be
		// rewritten: every prefab after each prefab it nests, ties in asset path order; `prefabs` themselves excluded.
		// Errors: Validation with the code PREFAB_CYCLE, naming the paths of a cycle, when the nesting graph has a cycle
		// through one of `prefabs` (one issue per prefab on it); NotFound for a handle that names no Prefab asset.
		[[nodiscard]] Result<std::vector<AssetHandle>> GetNestingPrefabs(std::span<const AssetHandle> prefabs) const;

		// The undoable command that rewrites the Prefab assets nesting `prefabs` (GetNestingPrefabs, in its order): when it
		// executes, each document's nested instances of a changed prefab are rebuilt from that prefab's version current then
		// (the asset commands composed before it have run), keeping the overrides the nesting prefab records for them, and
		// the document is written through the asset writer; undo restores each file. Nullptr when no prefab nests them.
		// prefab.apply and the other methods that give a prefab a new version compose their asset command, then this, then
		// CreatePrefabUpdateCommand of every prefab that changed, into one undo step. Errors: those of GetNestingPrefabs.
		[[nodiscard]] Result<Scope<Command>> CreateNestingPrefabUpdateCommand(std::span<const AssetHandle> prefabs);

		// The editor's revision (§13.4: "_meta".revision, ifRevision, the revisions in results and history entries, scene.diff
		// {against}). It increases with every mutation of the open scene and never repeats a value for the editor's lifetime,
		// across scene.open, scene.new, reloads and CloseScene, so an ifRevision taken in one scene can never match another
		// (no ABA): it is the open scene's own revision (Scene::GetRevision, which restarts with every Scene object) plus a
		// base that SetScene and CloseScene advance past every value reported so far. 0 before the first scene opens. During
		// a dry run it is the real revision plus the mutations the sandbox copy has seen, so a dry run reports what the real
		// call would produce (EditorDryRunScope adjusts the base while it swaps the scene).
		// Open scenes (M20, decision 13): the addressed scene's revision. One editor-wide counter, which every mutation of any
		// open scene advances as a mutation of the one open scene advances it before M20, gives each open scene its revision
		// at its last change, so a revision names one state of one scene and never matches another open scene's; with one
		// open scene the values are exactly those before M20.
		[[nodiscard]] uint64_t GetRevision() const;
		// The identity of the open scene document for services keyed to it (Autosave's source baseline): SetScene and
		// CloseScene advance it outside a dry run; edits, undo/redo and project-settings installations, which all advance
		// GetRevision, never do. 0 before the first scene opens.
		[[nodiscard]] uint64_t GetSceneDocumentEpoch() const { return m_SceneDocumentEpoch; }

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

		// M20 (decision 13): each open scene keeps its own selection, which survives switching tabs; these members read and
		// replace the addressed scene's, and play mode's selection belongs to the scene the session plays.
		//
		// The selected entities, in the order selected, without duplicates. A Play selection belongs to the runtime scene it
		// was made in (the session's serial and scene generation): once Scene.Load or a session replacement swaps that scene,
		// it reads as empty (ADR 0018: runtime scene replacement clears its selection), and PruneSelection drops it.
		[[nodiscard]] std::span<const UUID> GetSelection() const;
		// Replaces the selection in the shown scene (play while running, otherwise edit), recording that target.
		// Missing ids are dropped and duplicates removed (first kept).
		// Selection is editor state, not a command (it is never undone).
		void SetSelection(std::vector<UUID> selection);
		// M10 explicit target. Validates all ids in the chosen scene before replacing selection; InvalidState without
		// that scene, NotFound for a missing id. No dirty/history change; selection target prevents editing a UUID in the
		// wrong scene. Stop keeps the current selection's UUIDs that still exist in the edit scene; scene replacement clears it.
		[[nodiscard]] Status SetSelection(std::vector<UUID> selection, SceneTarget target);
		[[nodiscard]] SceneTarget GetSelectionTarget() const { return m_SelectionTarget; }
		// Play start and session replacement (EditorPlayController): an Edit selection follows into the new runtime scene by
		// UUID, keeping the ids that exist there (the mirror of Stop, §5.6), and a selection of the replaced runtime scene
		// is cleared. The target becomes Play; the selected asset is kept. No effect without a play session.
		void RetargetSelectionToPlay();
		// Restores a saved selection without failing: ids missing from `target`'s scene are dropped, and an absent target
		// scene clears the selection (Edit target). The selected asset is cleared only when an entity stays selected.
		void RestoreSelection(std::vector<UUID> selection, SceneTarget target);
		// Drops the selected ids that no longer exist in the target scene (destroyed entities, undone creations, runtime
		// destruction) and a selection of a replaced runtime scene. Once per frame (Update) and after UI actions; no effect
		// while a test run owns the editor, whose ReleaseTestRun restores the saved selection.
		void PruneSelection();

		// --- Test runs (§11.10, test.run) ----------------------------------------------------------------------------------

		// The one admission rule while a test run owns the editor, shared by automation requests and UI actions: only the
		// observation subset (state, logs, screenshots) is available, never a dry run. InvalidState names the method;
		// success when no test run is active.
		[[nodiscard]] Status CheckTestRunAdmission(std::string_view method, bool dryRun) const;

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
		// creation; the user-facing delete moves to the trash instead, AssetDeleteCommand). A directory is removed without
		// recursion (AssetWriter::RemoveEmptyDirectory), so one that still holds an entry the VFS does not list, a host name no
		// VfsPath can spell, is refused and nothing is deleted. During a dry run the removal happens in the overlay and nothing
		// is recorded. Errors: as MoveProjectFile; InvalidState for a non-empty directory, also one whose only entries the VFS
		// cannot list.
		[[nodiscard]] Status RemoveProjectFile(const VfsPath& path);

		// Creates a directory (and missing parents) under the project through the asset writer (asset.create {type: Folder},
		// moves into new folders). Errors: as MoveProjectFile.
		[[nodiscard]] Status CreateProjectDirectory(const VfsPath& directory);

		// The attribution of the request being served (set by AutomationServer::EnterInvocation, cleared by LeaveInvocation),
		// or nullopt for UI writes (Method "ui"). Each call, the UI's reset included, also makes a new write context current
		// in the asset manager (EditorAssetManager::SetWriteContext; Docs/Decisions/0026-m16-contract.md decision 10), so the
		// writes an import's publication makes are recorded with the attribution that was current when the import started,
		// even when the publication runs after the request returned (§13.12 rule 2).
		void SetWriteAttribution(std::optional<WriteAttribution> attribution);
		[[nodiscard]] const WriteAttribution& GetWriteAttribution() const { return m_Attribution; }
		// Agent while an automation request is served (an attribution is set), User otherwise.
		[[nodiscard]] CommandOrigin GetCommandOrigin() const { return m_HasRequestAttribution ? CommandOrigin::Agent : CommandOrigin::User; }
		// Main-thread editor policy. Denies persistent agent command/file mutations, including optional fixes in
		// otherwise read-only methods. Human actions and sandboxed dry runs remain available.
		void SetAgentMutationsDenied(bool denied) { m_AgentMutationsDenied = denied; }
		[[nodiscard]] bool AreAgentMutationsDenied() const { return m_AgentMutationsDenied; }

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

		// Shared CPU state of the UI and viewport methods. Main thread; valid for this context's lifetime. View changes
		// are session preferences, never a scene mutation or undo step. Existing screenshot camera access is preserved.
		[[nodiscard]] EditorViewportState& GetViewportState() { return m_ViewportState; }
		[[nodiscard]] const EditorViewportState& GetViewportState() const { return m_ViewportState; }
		[[nodiscard]] const ExplicitRenderCamera& GetSceneViewCamera() const { return m_ViewportState.GetCamera(); }
		[[nodiscard]] EditorUiState& GetUiState() { return m_UiState; }
		[[nodiscard]] const EditorUiState& GetUiState() const { return m_UiState; }

		// --- Shutdown (session.shutdown) --------------------------------------------------------------------------------

		// Asks the application to exit with `exitCode` after the current frame; the first request wins. EditorApp polls it at
		// its safe point.
		void RequestShutdown(int exitCode);
		[[nodiscard]] std::optional<int> GetShutdownRequest() const { return m_ShutdownRequest; }
	private:
		// Replaces the project settings with the canonical ProjectSerializer document `document` and writes it to the .eproj
		// through WriteProjectFile (§12.3). Private: settings change only through ProjectSettingsCommand, executed by Execute
		// (the class rule above). Errors: InvalidState without a project; Validation for an invalid document; the write
		// errors; nothing changed then. A successful installation advances the editor revision, including undo and redo.
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
		[[nodiscard]] Status CheckMutationPermission() const;
		// EditorAssetManager::OpenProject on the open project's project://Assets (created first when a writable project lacks
		// it) and cache://, read-only for a read-only project, with hot reload. Errors: those of the creation and of OpenProject.
		[[nodiscard]] Result<AssetRefreshReport> OpenProjectAssets();
		// OpenProject's --strict check (EditorContextSpecification::StrictUnknowns): every .scene and .prefab under
		// project://Assets, sorted by path, loaded strictly (LoadMode::Strict, LoadOptions::StrictUnknowns and the script field
		// schemas). Errors: Validation with one issue per file that does not load, whatever the reason, naming the file.
		[[nodiscard]] Status CheckProjectDocumentsStrictly() const;
		[[nodiscard]] Status RefreshScriptServices();
		[[nodiscard]] Status ReimportScriptForTest(AssetHandle script);
	private:
		EngineContext* m_Engine = nullptr; // documented back-reference: outlives the editor
		EditorContextSpecification m_Specification;
		EditorLifecycleCallbacks m_Lifecycle{};
		UUIDGenerator m_IdGenerator;
		// The asset services (M6), after the generator the manager draws handles from, so they are destroyed before it.
		Scope<ImporterRegistry> m_Importers;
		Scope<AssetLoaderRegistry> m_Loaders;
		Scope<EditorAssetManager> m_Assets;
		Scope<ScriptTypeChecker> m_ScriptChecker;
		Scope<EditorScriptService> m_ScriptService;
		uint64_t m_ScriptConfigurationHash = 0;
		double m_NextScriptConfigurationCheck = 0.0;
		std::string m_ScriptConfigurationError{};
		bool m_TestImportActive = false;
		Scope<AudioPreview> m_AudioPreview; // M12: after the manager it loads clips through, so it is destroyed first
		bool m_SceneChangedOnDisk = false;  // IsSceneChangedOnDisk
		Scope<LoadedProject> m_Project;
		Scope<Scene> m_Scene;
		std::optional<VfsPath> m_ScenePath;
		// GetSceneDocumentEpoch.
		uint64_t m_SceneDocumentEpoch = 0;
		bool m_SceneDirty = false;   // a dirty SetScene (a repaired load) not saved since; the history tracks the rest
		uint64_t m_RevisionBase = 0; // GetRevision's base, advanced by SetScene and CloseScene
		CommandHistory m_History;    // the open scene's history; a dry run swaps its sandbox history in
		std::vector<UUID> m_Selection;
		SceneTarget m_SelectionTarget = SceneTarget::Edit;
		uint64_t m_SelectionSession = 0;         // a Play selection's session serial (GetSelection)
		uint64_t m_SelectionSceneGeneration = 0; // and that session's scene generation
		WriteAttribution m_Attribution;
		bool m_HasRequestAttribution = false;
		bool m_AgentMutationsDenied = false;
		// The write contexts SetWriteAttribution made current (Docs/Decisions/0026-m16-contract.md decision 10), by Id, with the
		// attribution each stands for: kept while the token is current or an import holds it, pruned once it expired.
		struct WriteContextEntry
		{
			std::weak_ptr<const AssetWriteContext> Token{};
			WriteAttribution Attribution{};
		};
		std::map<uint64_t, WriteContextEntry> m_WriteContexts;
		uint64_t m_LastWriteContextId = 0;
		// OpenProject deferred the asset scan (EditorContextSpecification::DeferAssetScan) and OpenDeferredAssets has not run it
		// yet (IsAssetScanDeferred).
		bool m_AssetScanDeferred = false;
		std::optional<ProvenanceRecorder> m_Provenance;  // the open writable project's provenance
		std::optional<uint64_t> m_PendingRevisionBefore; // GetRevisionBeforeCommand's value while SceneEdit::Commit executes
		EditorTransaction* m_Transaction = nullptr;      // the outermost open transaction, which registers itself
		EditorDryRunScope* m_DryRun = nullptr;           // the open dry run, which registers itself
		std::optional<int> m_ShutdownRequest;
		// While a write path member that can change provenance (WriteProjectFile, MoveProjectFile, RemoveProjectFile) runs, the
		// write observer keeps the first provenance save failure here for that member to return; otherwise it logs it.
		bool m_CollectProvenanceErrors = false;
		std::optional<Error> m_ProvenanceError;
		EditorViewportState m_ViewportState{};
		EditorUiState m_UiState{};
		// The undo of each play-scene change a SceneEdit committed inside the open dry run or transaction (M7, §13.4: play-scene
		// edits are transient and never recorded, yet a dry run leaves no trace and a failed edit.batch rolls back every op):
		// run newest first when the dry run ends or the transaction rolls back, dropped when the outermost transaction commits
		// outside a dry run. SceneEdit::Commit appends them.
		std::vector<UniqueFunction<void()>> m_TransientPlayUndos;
		// Last, so it is destroyed first: a play session refers to the asset manager and the type registry (M7).
		Scope<EditorPlayController> m_Play;
	private:
		friend class CommandHistory;        // reads GetRevisionBeforeCommand; undo and redo check CheckMutationPermission
		friend class EditorFeatureTestHost; // synchronous, guarded Test.ReloadScript import only
		friend class EditorDryRunScope;
		friend class EditorSceneTargetScope; // M20: swaps the addressed scene in and out
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

	// Addresses one open scene for its lifetime (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 13). While it is
	// open, the members of EditorContext that name "the open scene" (its class comment lists them) address the scope's scene
	// instead of the active one, and Execute records in that scene's history; the active scene, the tab order and what the
	// editor shows do not change (but through ActivateScene and CloseScene of the scope's own scene, scene.activate's and
	// scene.close's subject), and the destructor makes the active scene the addressed one again. The automation server
	// opens one for a request of a method with MethodSpecification::SupportsSceneTarget that names a scene
	// (RequestOptions::Scene), for the request's whole stretch on the main thread (decision 16): AdmitRequest resolves the
	// target (EditorMethodContext::ResolveSceneTarget) and opens the scope before the deny-mutations discard check, the
	// ifRevision check and the dry-run sandbox, which therefore all read the target; FinishRequest closes the sandbox, records
	// the response's "_meta" state while the scope is still open, then closes the scope. So the handlers, the commands they
	// execute, entity paths and batch references, revisions, ifRevision, "_meta", the dry run's copy and the selection all
	// address the target without passing it on. A method with the flag is never pending (MethodSpecification), so the scope
	// never spans a frame. Main thread; scoped; not nested; not copyable or movable.
	class EditorSceneTargetScope
	{
	public:
		// Restricts construction to Begin; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class EditorSceneTargetScope;
		};

		// Use Begin. Records `context` and `scene` only; Begin then makes `scene` the addressed one.
		EditorSceneTargetScope(ConstructionKey key, EditorContext& context, OpenSceneId scene);

		// Opens a scope on `context` addressing the open scene `scene`. Errors: NotFound when no open scene has that id;
		// InvalidState inside a dry run, a transaction or another EditorSceneTargetScope (the server opens the scope outside
		// them, and scopes do not nest).
		[[nodiscard]] static Result<Scope<EditorSceneTargetScope>> Begin(EditorContext& context, OpenSceneId scene);

		~EditorSceneTargetScope();

		EditorSceneTargetScope(const EditorSceneTargetScope&) = delete;
		EditorSceneTargetScope& operator=(const EditorSceneTargetScope&) = delete;
		EditorSceneTargetScope(EditorSceneTargetScope&&) = delete;
		EditorSceneTargetScope& operator=(EditorSceneTargetScope&&) = delete;

		// The open scene this scope addresses.
		[[nodiscard]] OpenSceneId GetScene() const;
	private:
		// The context back-reference, the addressed scene and what the scope swapped out (EditorContext.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
