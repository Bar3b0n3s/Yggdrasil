#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>

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
	// One edit at a time per scene (the change tracker is not reentrant, asserted). Main thread only. Not copyable or
	// movable.
	class SceneEdit
	{
	public:
		// Begins tracking changes to `context`'s open scene (asserted present, and no other edit active). `label` is the undo
		// label without the origin prefix; `mergeKey` as in SceneEditCommand.
		SceneEdit(EditorContext& context, std::string label, std::string mergeKey = {});
		// Rolls back when still active (Cancel).
		~SceneEdit();

		SceneEdit(const SceneEdit&) = delete;
		SceneEdit& operator=(const SceneEdit&) = delete;
		SceneEdit(SceneEdit&&) = delete;
		SceneEdit& operator=(SceneEdit&&) = delete;

		// Ends tracking and records the edit: builds a SceneEditCommand from the tracker's changes (Before snapshots) and the
		// scene's current state (After), then hands it to EditorContext::Execute, which records it in the history, appends it
		// to the open transaction, or discards it after a dry run. Returns the undo index (CommandHistory sequence; 0 for a
		// dry run, and when nothing changed, in which case nothing is recorded). In Debug builds, also asserts that the state
		// hash of every entity the tracker did not report is unchanged (§12.3). Errors: Validation when an After state cannot
		// be serialized (a non-finite value written through Patch); the edit is rolled back then.
		[[nodiscard]] Result<uint64_t> Commit();

		// Ends tracking and restores every touched entity to its Before state. Idempotent; nothing after Commit.
		void Cancel();

		// The scene being edited.
		[[nodiscard]] Scene& GetScene() const;
		// True until Commit or Cancel.
		[[nodiscard]] bool IsActive() const { return m_IsActive; }
	private:
		EditorContext* m_Context = nullptr; // documented back-reference: outlives the edit
		std::string m_Label;
		std::string m_MergeKey;
		bool m_IsActive = true;
	};

}
