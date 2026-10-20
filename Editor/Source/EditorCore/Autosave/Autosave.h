#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Engine {

	class EditorContext;
	class LoadedProject;

	enum class AutosaveReason : uint8_t
	{
		Periodic,
		BeforePlay,
		FatalError,
		Exit // the final generation when the user closes the editor without saving its scene
	};
	enum class AutosaveFatalResult : uint8_t
	{
		Saved,
		NothingDirty,
		Disabled,
		Busy,
		IoFailure
	};

	struct AutosaveSpecification
	{
		double IntervalSeconds = 120.0;
		uint32_t RetainedGenerations = 3;
		// Optional native atomic writer; empty uses FileSystem::WriteFileAtomic without backups. Copied into the service.
		// Called under the sole writer lease, on either the main or failing thread. Production callbacks must never
		// access editor/ECS/VFS/GPU state, wait on an application lock, or log. Captures must outlive this service.
		// Expected failures return Status. An exception on the fatal path becomes IoFailure without logging.
		std::function<Status(const std::filesystem::path&, std::span<const std::byte>)> WriteFile{};
		// Optional observer of every native file whose bytes the service reads, called before the read with the same
		// thread and lifetime rules as WriteFile (tests count reads). It cannot change the read.
		std::function<void(const std::filesystem::path&)> ReadObserver{};
	};

	struct AutosaveRecoveryInfo
	{
		std::string Generation{}; // opaque generation ID, never used as an unchecked native path
		std::string ScenePath{};  // project-relative, empty for an untitled scene
		std::string SceneName{};
		uint64_t SceneRevision = 0;
		std::vector<std::string> Files{}; // confined project-relative destinations, sorted
		bool NewerThanSource = false;     // dirty revision derived from an unchanged fingerprint, not a timestamp comparison
	};

	struct AutosaveWriteResult
	{
		bool Written = false;
		std::string Generation{};
		std::vector<std::string> Files{};
	};

	// CPU-only, owned by the host while EditorContext outlives it. The host uninstalls/quiesces the fatal hook before
	// destruction. Normal functions are main-thread only; WriteFatalSnapshot is the sole any-thread entry point.
	// Reset closes writer admission atomically with the fatal claim. Until Reset returns true the host retains the
	// current LoadedProject and its writer lock, this service and every claimed slot; project replacement cannot proceed.
	//
	// Publish captures a strictly serialized immutable scene at safe points. Native assets/settings already write through
	// their commands, so there is no fabricated dirty-asset store. A pending drawer edit is uncommitted and excluded;
	// gizmo preview is likewise uncommitted and excluded. Finish a UI gesture before BeforePlay.
	//
	// Worker fatal path claims a published immutable slot, never reads ECS, calls VFS, takes/waits on a mutex or GPU.
	// Slots own bytes and resolved native paths prepared on the main thread; publication uses atomic state transitions.
	// Keep the last published generation usable while preparing the next. Fixed bounded slots; no unbounded snapshots.
	// One atomic disk-writer claim covers payloads, manifest publication and retention. Fatal claims never wait; a normal
	// save already writing means Busy. The claim keeps the closing project pinned through the host Reset protocol.
	// The host publishes after committed edits and before GPU work. Every fatal callback uses that last published
	// revision through WriteFatalSnapshot, including main-thread GPU failures; in-flight edits may be absent.
	//
	// Writes use generation directories under Library/Autosave and atomically publish the manifest LAST. Incomplete
	// generations are ignored. Read-only/dry-run/no-project states never publish paths or write into the project.
	// OS faults/signals do not call this service. Fatal writes can fail (OOM/I/O); failure never recurses into FatalError.
	//
	// Open scenes (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 15; stream S-A). Autosave is isolated per open
	// scene: every member above that names "the scene" applies to each open scene on its own. Publish keeps one immutable
	// snapshot slot per open scene (at most MaxOpenScenes, so the fatal path's slots stay fixed and bounded), each keyed by
	// its scene document (path plus EditorContext's document epoch for that scene); a clean scene withdraws only its own.
	// One generation holds one scene, as before M20: Save, and WriteFatalSnapshot from every published slot, write one
	// generation per dirty open scene, in tab order, and publish the manifest once, last; retention keeps
	// RetainedGenerations per scene. AutosaveWriteResult::Files lists every scene written, in tab order, and Generation is
	// the last generation written. FindRecovery keeps offering the newest recoverable generation (the last of
	// FindRecoveries, which offers one per scene); Recover installs its scene as the open scene of that source (replacing it
	// when it is open, else as an additional open scene, EditorContext::AddScene), dirty with an empty history;
	// DiscardSavedRecovery after an explicit save discards only the saved scene's generations. The generation and manifest
	// formats are unchanged.
	class Autosave
	{
	public:
		explicit Autosave(EditorContext& context, const AutosaveSpecification& specification = {});
		~Autosave();
		Autosave(const Autosave&) = delete;
		Autosave& operator=(const Autosave&) = delete;

		// Injected monotonic, unscaled seconds, starting at zero for a new project. InvalidArgument for negative/non-finite
		// or decreasing time. At >= last attempt + IntervalSeconds, Save(Periodic) once, never a catch-up burst.
		// Read-only/dry-run/no-project return Written=false without logging; autosave is off in these states.
		// Errors propagate (Io/Validation); a failed attempt does not mark the scene saved or remove older recoveries.
		[[nodiscard]] Result<AutosaveWriteResult> Update(double nowSeconds);
		// Refresh immutable bytes when revision/path/project fingerprint/dirty state changed. Called once per host safe point
		// (through Update) and before request-driven GPU work; clean/read-only/dry-run/no-project withdraw publication and
		// succeed without logging/writing. Cheap when nothing changed: the scene document, revision, dirty and on-disk
		// flags plus the project and source files' size and opaque write time key the last outcome (success or Conflict),
		// which is returned again without canonicalizing paths, serializing or reading file bytes. A file whose size and
		// write time are unchanged since its last read keeps its hash; each generation write verifies the bytes once.
		// The document is EditorContext::GetSceneDocumentEpoch plus the scene path: settings changes keep its baseline. A
		// new document's baseline is the source as it is now, unless the scene already changed on disk (dirty: Conflict).
		// The project's captured fingerprint follows any .eproj whose bytes equal the canonical serialization of the loaded
		// settings (the editor's own writes); other changes are Conflict until the project is reopened.
		// InvalidState while Reset is pending. After a successful Reset, the host may Publish for a newly opened project
		// (or the same one if close was cancelled), beginning a new admission epoch; it never reuses an old claimed slot.
		[[nodiscard]] Status Publish();
		// Capture current dirty edit scene, then atomic generation write; clean/no-project gives Written=false.
		// PermissionDenied read-only, InvalidState dry-run, Validation serialization or invalid settings, Io.
		// Does not clear scene dirty state, mutate history, write Assets/ or change provenance.
		// InvalidState while Reset is pending or another disk writer is active; never contend with a fatal writer.
		// FatalError and Exit never duplicate the newest generation this service wrote for the same published snapshot
		// (Written=false then). Retiring old generations after the manifest is published is best effort; a later write
		// sweeps generation directories the manifest no longer names.
		[[nodiscard]] Result<AutosaveWriteResult> Save(AutosaveReason reason);
		// Any thread, bounded nonblocking claim of both the published slot and sole disk writer, no state access.
		// Disabled after Reset closes admission; Busy if the claim fails or another writer is active. Never waits/retries
		// for Reset, another writer or a slot. An earlier successful claim may finish during Reset; its project lock stays
		// held until Reset reports quiescence. All outcomes release the claim; normal logging is forbidden. Saved without
		// writing when the claimed snapshot is already the manifest's newest generation written by this service.
		[[nodiscard]] AutosaveFatalResult WriteFatalSnapshot() noexcept;
		// Inspect only a valid complete generation for this canonical .eproj path and captured project fingerprint.
		// The formats have no project/scene UUID: compare scene path, source hash, size and write time, plus format/version.
		// Changed/replaced/recreated project or source files are conflicts, never silently adopted. Verify payload hashes,
		// version and confined paths before offering it. A complete dirty revision based on an unchanged fingerprint is
		// newer; a clean/byte-identical payload is not. ModificationTime supports equality only, never numeric ordering.
		// Order complete generations using the persisted manifest sequence, never timestamps or directory iteration.
		// Reopening proves freshness from the stored base fingerprint and dirty revision, not process-local scene counters.
		// Untitled scene is recoverable. Malformed manifests return Parse/Validation; no source files change.
		[[nodiscard]] Result<std::optional<AutosaveRecoveryInfo>> FindRecovery() const;
		// M20 (decision 15): FindRecovery's offer for every scene with a recoverable generation, one per scene source (its
		// canonical path, or the untitled token), each the newest of its scene, ordered by the persisted manifest sequence of
		// that newest generation (oldest first, so the scenes of one save come back in the tab order they were written in).
		// Empty when nothing is recoverable. Errors: those of FindRecovery.
		[[nodiscard]] Result<std::vector<AutosaveRecoveryInfo>> FindRecoveries() const;
		// Validate all bytes into temporary CPU objects first, then install the recovered scene once, dirty, empty history.
		// Never overwrite source Assets files. A failure leaves current scene/project data untouched. Requires the held
		// writer lock: PermissionDenied read-only; InvalidState while a test run owns the editor; NotFound stale
		// generation; Conflict source changed since offer.
		[[nodiscard]] Status Recover(const AutosaveRecoveryInfo& recovery);
		// Main-thread launcher operation. Takes the already-locked project and preflights the recovery into temporary
		// CPU objects, including its original source fingerprint/untitled token, before opening any editor mounts.
		// Requires this service to be quiescent/unbound (new, or after successful Reset); it never closes a project or
		// changes host lifecycle policy. InvalidArgument for null; PermissionDenied read-only; InvalidState if already open,
		// in a dry run or busy;
		// otherwise the validation/read errors of Recover and EditorContext::OpenProject. On failure the editor remains
		// in the launcher and the transferred project lock is released. On success installs once, dirty, empty history,
		// retaining the validated baseline in this same long-lived service for matching cleanup after explicit save.
		[[nodiscard]] Status OpenRecoveredProject(Scope<LoadedProject> project, const AutosaveRecoveryInfo& recovery);
		// M20 (decision 15): OpenRecoveredProject for several scenes (FindRecoveries' offers, in their order): every one is
		// preflighted before anything opens, then each opens as an open scene, dirty with an empty history, in the given
		// order, and the first becomes active. A failure of any leaves the editor in the launcher, as above. Errors: those
		// above; InvalidArgument for an empty list, two offers of one scene source or more than MaxOpenScenes offers.
		[[nodiscard]] Status OpenRecoveredProject(Scope<LoadedProject> project, std::span<const AutosaveRecoveryInfo> recoveries);
		// After successful explicit save, discard only generations whose source matches the saved canonical path and
		// captured source fingerprint (untitled uses the captured in-session document token until first save).
		// Never discard another scene's recovery: a generation that cannot be read or belongs to another canonical project
		// (a moved project) is kept, never an error. Removing directories after the manifest is rewritten is best effort.
		// InvalidState dry-run; PermissionDenied read-only; Io.
		[[nodiscard]] Status DiscardSavedRecovery();
		// Main thread, nonblocking close handshake: atomically stop new writer claims and withdraw publication. Returns
		// false if a prior claim is still writing; the host retries at later safe points and MUST keep the old project/lock
		// and this service alive meanwhile. True means no old writer can still publish, reclaim or access its project.
		// Idempotent; never reopens admission. Do not spin, unlock, unmount or replace the project after a false result.
		// Shutdown first quiesces/uninstalls the fatal hook, then requires a successful Reset before destruction.
		[[nodiscard]] bool Reset();
	private:
		struct State;
		Scope<State> m_State{};
	};

}
