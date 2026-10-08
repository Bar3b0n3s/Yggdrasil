#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Scene/ChangeTracker.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	class EditorContext;
	class Scene;

	// The way every scene operation becomes one undo step (§12.3):
	//     SceneEdit edit(context, "Reparent 'Board'");          // begins tracking
	//     ENGINE_TRY(scene.SetParent(child, parent));            // mutate through the normal Scene/Entity/ComponentAccess APIs
	//     ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit()); // one SceneEditCommand, executed through the context
	// An edit that is neither committed nor cancelled when it goes out of scope (an early return through ENGINE_TRY) is
	// rolled back, so a failed operation leaves the scene exactly as it was: atomicity comes for free.
	//
	// Play mode (§5.6, §13.4; M7). While the editor plays, the edit also tracks the play session's scene, so a method that
	// changes the play scene (target "play") goes through the same machinery: its play-scene changes are transient. Commit
	// records no command for them (they are not undoable and Stop discards them with the session) and reports undo index 0
	// for an edit that changed only the play scene; a failed or cancelled edit rolls them back like edit-scene changes.
	// Inside a dry run or a transaction Commit keeps their undo with the editor, so the dry run's end and the transaction's
	// rollback (edit.batch's failed op) take them back too: a dry run never changes the play scene. Whatever takes the
	// play-scene changes back also winds the session's UUIDGenerator back to where the edit found it: a play-scene creation
	// draws a runtime id from it (§4.8), and its draw count is part of the state hash, so an edit that is taken back leaves
	// no trace in the session (§13.4) and the next creation gets the id it would have got without it.
	//
	// One edit at a time per scene (the change tracker is not reentrant, asserted). Main thread only. Not copyable or
	// movable.
	class SceneEdit
	{
	public:
		// Begins tracking changes to `context`'s open scene and, while the editor plays, to the play session's scene (at least
		// one of them asserted present, and no other edit active on either). `label` is the undo label without the origin
		// prefix; `mergeKey` as in SceneEditCommand.
		SceneEdit(EditorContext& context, std::string label, std::string mergeKey = {});
		// Rolls back when still active (Cancel).
		~SceneEdit();

		SceneEdit(const SceneEdit&) = delete;
		SceneEdit& operator=(const SceneEdit&) = delete;
		SceneEdit(SceneEdit&&) = delete;
		SceneEdit& operator=(SceneEdit&&) = delete;

		// Ends tracking and records the edit. First, while the tracker still runs, it refreshes the override records of every
		// prefab instance of the edit scene whose root or members the edit touched against the prefab's current version (§5.5
		// "the change tracker records field-level overrides"; PrefabInstantiator::RefreshOverrides), so they are part of the
		// same step and rebuilding the instance from prefab + overrides keeps the edit; an instance whose prefab is not
		// registered keeps its records. It then builds a SceneEditCommand from the tracker's changes of the edit scene (Before
		// snapshots) and the scene's current state (After), then hands it to EditorContext::Execute, which records it in the
		// history, appends it to the open transaction, or discards it after a dry run, and appends the edit's events
		// (SceneEditCommand::AppendChangeEvents). Changes of the play scene are kept as they are (see the class comment).
		// Returns the undo index (CommandHistory sequence; 0 for a dry run, and when the edit scene did not change, in which
		// case nothing is recorded). In Debug builds, also asserts that the state hash of every entity the tracker did not
		// report is unchanged (§12.3), in both scenes. Errors: Validation when an After state cannot be serialized (a
		// non-finite value written through Patch); the edit is rolled back then, in both scenes.
		[[nodiscard]] Result<uint64_t> Commit();

		// Ends tracking and restores every touched entity to its Before state, in both scenes. Idempotent; nothing after Commit.
		void Cancel();

		// The edit scene being edited (asserted open).
		[[nodiscard]] Scene& GetScene() const;
		// True until Commit or Cancel.
		[[nodiscard]] bool IsActive() const { return m_IsActive; }
	private:
		// One scene the edit tracks: the open edit scene, and the play session's scene while the editor plays.
		struct TrackedScene
		{
			Scene* Target = nullptr; // null when the edit does not track this scene
			// Debug builds: every entity's canonical JSON when the edit began, for Commit's check of the untouched ones (§12.3).
			std::vector<std::pair<UUID, Json>> EntityStates;

			// Begins tracking `scene` (asserted: no other edit tracks it).
			void Begin(Scene& scene, std::string_view label);
			// Ends tracking and returns the tracker's changes (none when the scene is not tracked); in Debug builds, first asserts
			// that every entity the tracker did not report is unchanged.
			[[nodiscard]] std::vector<EntityChange> End(std::string_view label);
		};

		// Winds the play session's UUIDGenerator back to m_PlayIdsBefore, when the edit tracks the play scene and its session
		// still runs.
		void RestorePlayIds() const;
	private:
		EditorContext* m_Context = nullptr; // documented back-reference: outlives the edit
		std::string m_Label;
		std::string m_MergeKey;
		uint64_t m_RevisionBefore = 0; // EditorContext::GetRevision when the edit began: the history entry's RevisionBefore
		TrackedScene m_EditScene;
		TrackedScene m_PlayScene;
		// While the edit tracks the play scene: the session's serial (PlaySession::GetSerial: undos run later check that they
		// still address it) and its UUIDGenerator when the edit began.
		uint64_t m_PlaySerial = 0;
		std::optional<UUIDGenerator> m_PlayIdsBefore;
		bool m_IsActive = true;
	};

}
