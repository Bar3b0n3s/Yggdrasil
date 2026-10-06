#pragma once

#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class Scene;

	// What one tracked edit did to one entity (§12.3): its canonical entity JSON before and after
	// (SceneSerializer::EntityToJson; null = the entity did not exist), and its place in the hierarchy before and after. Its
	// sibling indexes are relative to the parent's child list at the start of the edit (Before) and at its end (After), so
	// restoring every touched entity of one side in ascending index order reproduces that side's child order exactly (ADR
	// 0006 decision 17).
	struct SceneEntityChange
	{
		UUID EntityID;
		Ref<const Json> Before; // null: created by the edit
		Ref<const Json> After;  // null: destroyed by the edit
		UUID ParentBefore;      // the invalid UUID for a root (and for a created entity)
		uint32_t SiblingIndexBefore = 0;
		UUID ParentAfter; // the invalid UUID for a root (and for a destroyed entity)
		uint32_t SiblingIndexAfter = 0;
	};

	// The generic snapshot-undo command (§12.3): almost every scene operation is one, built by SceneEdit::Commit from the
	// scene's change tracker. Undo restores every touched entity's Before state and redo its After state, through the
	// serializer (SceneSerializer::EntityFromJson, ApplyEntityJson, Scene::DestroyEntity, Scene::SetParent), which is the
	// one scene-copy path (§1.3). Restoring creates parents before children, destroys children before parents, and inserts
	// siblings in ascending index order. Changes are kept sorted by UUID (ChangeTracker::End).
	//
	// Property (Roadmap M4): after any sequence of operations, undoing everything restores the original scene
	// byte-identically and redoing everything reproduces the final scene.
	class SceneEditCommand final : public Command
	{
	public:
		// A command in its applied state (the edit has happened): the first Execute changes nothing. `changes` must not be
		// empty and every change must have Before or After (asserted). `mergeKey` (may be empty) enables merging with the
		// next edit of the same key (continuous edits: one gizmo drag, one slider scrub).
		SceneEditCommand(std::string label, std::vector<SceneEntityChange> changes, std::string mergeKey = {});

		// Brings every touched entity to its After state (nothing when the command is applied). Errors: Validation when a
		// snapshot no longer applies (a bug elsewhere: undo states are always valid), with the scene restored to the Before
		// state first; the command stays unapplied.
		[[nodiscard]] Status Execute(EditorContext& context) override;
		// Brings every touched entity back to its Before state. An in-memory restore: it always succeeds (a failure is a bug,
		// asserted, Command.h), so the returned Status is always success.
		[[nodiscard]] Status Undo(EditorContext& context) override;

		[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
		[[nodiscard]] std::string_view GetMergeKey() const override { return m_MergeKey; }
		// Merges a later SceneEditCommand of the same key: keeps this command's Before states, takes the other's After states
		// and adds the entities only the other touched.
		[[nodiscard]] bool MergeWith(const Command& next) override;
		[[nodiscard]] size_t GetMemorySize() const override;

		[[nodiscard]] std::span<const SceneEntityChange> GetChanges() const { return m_Changes; }
		[[nodiscard]] bool IsApplied() const { return m_IsApplied; }

		// Brings `scene`'s touched entities to the Before (`after` false) or After (`after` true) state of `changes`, which
		// must be the changes of one edit, applied to a scene in the opposite state. Used by Execute and Undo, and by
		// scene.diff, which replays history changes on a scratch copy. Errors: Validation (nothing is rolled back: callers
		// apply to scenes they can discard or restore).
		[[nodiscard]] static Status ApplyChanges(Scene& scene, std::span<const SceneEntityChange> changes, bool after);
	private:
		std::string m_Label;
		std::vector<SceneEntityChange> m_Changes;
		std::string m_MergeKey;
		bool m_IsApplied = true;
	};

}
