#pragma once

#include "Engine/Core/Result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	class EditorContext;

	enum class AutosaveReason : uint8_t
	{
		Periodic,
		BeforePlay,
		FatalError
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
	// Main-thread GPU failures at a known safe boundary call Save(FatalError) first to capture the newest committed scene;
	// all other fatal paths use the last published revision (best effort, explicitly allowed to lag in-flight edits).
	//
	// Writes use generation directories under Library/Autosave and atomically publish the manifest LAST. Incomplete
	// generations are ignored. Read-only/dry-run/no-project states never publish paths or write into the project.
	// OS faults/signals do not call this service. Fatal writes can fail (OOM/I/O); failure never recurses into FatalError.
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
		// Refresh immutable bytes when revision/path/project fingerprint/dirty state changed. Called after automation/UI commits and
		// before GPU work; clean/read-only/dry-run/no-project withdraw publication and succeed without logging/writing.
		// InvalidState while Reset is pending. After a successful Reset, the host may Publish for a newly opened project
		// (or the same one if close was cancelled), beginning a new admission epoch; it never reuses an old claimed slot.
		[[nodiscard]] Status Publish();
		// Capture current dirty edit scene, then atomic generation write; clean/no-project gives Written=false.
		// PermissionDenied read-only, InvalidState dry-run, Validation serialization or invalid settings, Io.
		// Does not clear scene dirty state, mutate history, write Assets/ or change provenance.
		// InvalidState while Reset is pending or another disk writer is active; never contend with a fatal writer.
		[[nodiscard]] Result<AutosaveWriteResult> Save(AutosaveReason reason);
		// Any thread, bounded nonblocking claim of both the published slot and sole disk writer, no state access.
		// Disabled after Reset closes admission; Busy if the claim fails or another writer is active. Never waits/retries
		// for Reset, another writer or a slot. An earlier successful claim may finish during Reset; its project lock stays
		// held until Reset reports quiescence. All outcomes release the claim; normal logging is forbidden.
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
		// Validate all bytes into temporary CPU objects first, then install the recovered scene once, dirty, empty history.
		// Never overwrite source Assets files. A failure leaves current scene/project data untouched. Requires the held
		// writer lock: PermissionDenied read-only; NotFound stale generation; Conflict source changed since offer.
		[[nodiscard]] Status Recover(const AutosaveRecoveryInfo& recovery);
		// After successful explicit save, discard only generations whose source matches the saved canonical path and
		// captured source fingerprint (untitled uses the captured in-session document token until first save).
		// Never discard another scene's recovery. InvalidState dry-run; PermissionDenied read-only; Io.
		[[nodiscard]] Status DiscardSavedRecovery();
		// Main thread, nonblocking close handshake: atomically stop new writer claims and withdraw publication. Returns
		// false if a prior claim is still writing; the host retries at later safe points and MUST keep the old project/lock
		// and this service alive meanwhile. True means no old writer can still publish, reclaim or access its project.
		// Idempotent; never reopens admission. Do not spin, unlock, unmount or replace the project after a false result.
		// Shutdown first quiesces/uninstalls the fatal hook, then requires a successful Reset before destruction.
		[[nodiscard]] bool Reset();
	};

}
