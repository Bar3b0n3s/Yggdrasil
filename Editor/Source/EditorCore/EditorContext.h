#pragma once

#include "EditorCore/Automation/ProvenanceRecorder.h"
#include "EditorCore/Commands/Command.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VfsPath.h"
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

	class EditorDryRunScope;
	class EditorTransaction;
	class EngineContext;
	class IMount;
	class ProjectSettingsCommand;
	class SceneEdit;
	class TypeRegistry;
	class VirtualFileSystem;

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

		// --- Project -------------------------------------------------------------------------------------------------------

		[[nodiscard]] bool HasProject() const { return m_Project != nullptr; }
		// The open project (asserted HasProject).
		[[nodiscard]] const LoadedProject& GetProject() const;
		// The open project was opened with --read-only; false without a project.
		[[nodiscard]] bool IsReadOnly() const;

		// Takes the opened project (ProjectManager::OpenProject): mounts project:// at its root (keeping .bak files, read-only
		// for a read-only project) and cache:// at its cache directory, loads its provenance (none for read-only projects),
		// adds it to the recent list (when user:// is mounted; a failure there is logged, not returned) and starts with no
		// open scene and an empty history. Errors: InvalidState when a project is already open; the mount errors and
		// ProvenanceRecorder::Load errors, after which nothing changed.
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

		// The open scene was written to `path` (scene.save, project.save): sets the scene path and the save point.
		void MarkSceneSaved(const VfsPath& path);

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
	private:
		EngineContext* m_Engine = nullptr; // documented back-reference: outlives the editor
		EditorContextSpecification m_Specification;
		UUIDGenerator m_IdGenerator;
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
	private:
		friend class CommandHistory; // reads GetRevisionBeforeCommand
		friend class EditorDryRunScope;
		friend class EditorTransaction;
		friend class ProjectSettingsCommand; // the one caller of ApplyProjectSettings
		friend class SceneEdit;              // sets m_PendingRevisionBefore around its Execute
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
