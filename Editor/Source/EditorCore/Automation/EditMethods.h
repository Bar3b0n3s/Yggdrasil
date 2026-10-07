#pragma once

#include "EditorCore/Automation/AutomationTypes.h"
#include "EditorCore/Commands/Command.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

// edit.* (Architecture §13.4, §13.5): atomic batches, undo and redo, the history and the selection. Conventions as in
// MethodRegistry.h.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// Registry struct "EditBatchOp": {method, params}.
	struct EditBatchOp
	{
		std::string Method{};
		VariantValue Params{}; // an object (or null for {}); may hold {"$ref": "<op index>.<path>"} (JsonReference.h)
	};

	// edit.batch {label?, ops, dryRun?} (§13.4 "Atomic batch"): runs the ops in order inside one EditorTransaction (one
	// CompositeCommand, one undo step labelled `label`), each through MethodRegistry::InvokeNested after substituting "$ref"
	// values from the earlier ops' results (indexBase 0). If op k fails, ops 0 .. k-1 are rolled back and the call fails with
	// op k's error, whose data carries "failedOp": k; a rollback that cannot complete (EditorTransaction::Rollback) adds its
	// error as a context and data.rollbackFailed: true.
	//
	// Before the first op runs, every op is checked and the batch is rejected with "failedOp" naming the first bad one:
	//   - an unknown method (NotFound);
	//   - a method that is not AllowedInBatch (InvalidArgument): edit.batch itself, every pending method, and the methods
	//     that replace the open scene, write files outside a command or end the session (MethodRegistry.h);
	//   - op params that are not an object, or that carry dryRun, ifRevision or _meta (InvalidArgument located at
	//     "/ops/<k>/params/<member>": a batch's reserved members apply to all of its ops and are given once, on the batch);
	//   - in a dry-run batch, an op without supportsDryRun (Unsupported).
	// With dryRun the whole batch runs in the dry-run sandbox; every op sees the batch's options.
	struct EditBatchParams
	{
		std::string Label = "Batch";
		std::vector<EditBatchOp> Ops{}; // 1 to 1000 ops
	};

	struct EditBatchResult
	{
		std::vector<VariantValue> Results{}; // each op's result, in op order
		uint32_t UndoIndex = 0;
	};

	// edit.undo {steps?}.
	struct EditUndoParams
	{
		uint32_t Steps = 1; // 1 to 1000
	};

	// edit.undo stops at a command whose Undo fails (a settings command's file write, Command.h): the call then fails with
	// that error, data.undone holding the number undone before it, and the failed command stays applied.
	struct EditUndoResult
	{
		uint32_t Undone = 0;     // fewer than steps when the history ran out
		uint32_t UndoIndex = 0;  // the newest applied command afterwards (0: none)
		std::string UndoLabel{}; // what a further edit.undo would undo
		std::string RedoLabel{};
	};

	// edit.redo {steps?}.
	struct EditRedoParams
	{
		uint32_t Steps = 1; // 1 to 1000
	};

	struct EditRedoResult
	{
		uint32_t Redone = 0;
		uint32_t UndoIndex = 0;
		std::string UndoLabel{};
		std::string RedoLabel{};
	};

	// edit.history {limit?}.
	struct EditHistoryParams
	{
		uint32_t Limit = 100; // 1 to 1000
	};

	// Registry struct "EditHistoryEntry".
	struct EditHistoryEntry
	{
		uint32_t UndoIndex = 0;                     // CommandHistoryEntry::Sequence
		std::string Label{};                        // with the "[agent] " prefix for agent commands
		CommandOrigin Origin = CommandOrigin::User; // registry enum "CommandOrigin"
		uint32_t RevisionBefore = 0;
		uint32_t RevisionAfter = 0;
		bool Applied = true; // false for the redo branch
	};

	struct EditHistoryResult
	{
		std::vector<EditHistoryEntry> Entries{}; // oldest first, ending with the redo branch
		uint32_t UndoIndex = 0;
		bool Dirty = false;
	};

	// edit.select {entities}: replaces the selection (editor state, not undoable, and therefore not flagged Mutates).
	struct EditSelectParams
	{
		std::vector<std::string> Entities{}; // EntityRefs; empty clears the selection
	};

	struct EditSelectResult
	{
		std::vector<EntitySummary> Selection{};
	};

	// edit.getSelection {}.
	struct EditGetSelectionResult
	{
		std::vector<EntitySummary> Selection{};
	};

	namespace Automation {

		// edit.batch. Errors: the failing op's error with data.failedOp; InvalidArgument for an empty batch, an op that is
		// not AllowedInBatch, reserved members in op params or a malformed $ref; Unsupported for a dry-run batch with an op
		// that cannot dry-run.
		[[nodiscard]] Result<EditBatchResult> EditBatch(EditorMethodContext& context, const EditBatchParams& params);
		// edit.undo. Errors: a command whose Undo fails, with data.undone (it and everything older stay applied).
		[[nodiscard]] Result<EditUndoResult> EditUndo(EditorMethodContext& context, const EditUndoParams& params);
		// edit.redo. Errors: a command that fails to redo (the rest stays undone).
		[[nodiscard]] Result<EditRedoResult> EditRedo(EditorMethodContext& context, const EditRedoParams& params);
		[[nodiscard]] Result<EditHistoryResult> EditHistory(EditorMethodContext& context, const EditHistoryParams& params);
		// edit.select. Errors: NotFound for a reference naming no entity (the selection is unchanged then).
		[[nodiscard]] Result<EditSelectResult> EditSelect(EditorMethodContext& context, const EditSelectParams& params);
		[[nodiscard]] Result<EditGetSelectionResult> EditGetSelection(EditorMethodContext& context, const NoParams& params);

	}

	void RegisterEditMethodTypes(TypeRegistry& registry);

	// Registers the six methods: edit.batch, edit.undo and edit.redo are tools (§13.8) and mutate; edit.batch supports dry
	// runs (§13.4); edit.history, edit.select and edit.getSelection are not tools; only edit.history and edit.getSelection
	// are AllowedInBatch (edit.select changes editor state that a rollback cannot restore).
	void RegisterEditMethods(MethodRegistry& methods);

}
