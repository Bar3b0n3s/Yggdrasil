#include "EditorPCH.h"
#include "EditorCore/Automation/EditMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/JsonReference.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		// edit.batch's bound on its ops (EditBatchParams::Ops), and edit.undo's and edit.redo's on their steps.
		constexpr size_t MaxBatchOps = 1000;
		constexpr uint32_t MaxSteps = 1000;

		// The registry enforces the steps range when it reads the params; the history asserts it, so it is checked here too.
		static Status CheckSteps(uint32_t steps)
		{
			if (steps == 0 || steps > MaxSteps)
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/steps", std::format("steps must be 1 to {} (got {})", MaxSteps, steps)));
			return {};
		}
		// The params members a batch op never carries: they are given once, on the batch (MethodRegistry::InvokeNested).
		constexpr std::array<std::string_view, 3> ReservedOpMembers = { "dryRun", "ifRevision", "_meta" };

		// `error` of op `index` relocated into the batch's params: pointers relative to the op's params gain
		// "/ops/<index>/params", and those InvokeNested already made relative to the op ("/params/...") gain "/ops/<index>".
		static Error LocateOpError(const Error& error, size_t index)
		{
			const std::string opPointer = std::format("/ops/{}", index);
			const std::string paramsPointer = opPointer + "/params";
			const auto relocate = [&opPointer, &paramsPointer](const std::string& pointer)
			{
				return pointer == "/params" || pointer.starts_with("/params/") ? opPointer + pointer : paramsPointer + pointer;
			};

			std::vector<ErrorIssue> issues = error.GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = relocate(issue.JsonPointer);
			Error relocated = ReplaceIssues(error, std::move(issues));
			if (error.GetLocation().JsonPointer.has_value())
			{
				ErrorLocation location;
				location.JsonPointer = relocate(*error.GetLocation().JsonPointer);
				return std::move(relocated).WithLocation(std::move(location));
			}
			return relocated;
		}

		// The checks every op passes before the batch runs anything (§13.4: "rejects a batch ... before running anything").
		// Errors are located at the op.
		static Status CheckBatchOp(const MethodRegistry& registry, const EditBatchOp& op, size_t index, bool dryRun)
		{
			const std::string methodPointer = std::format("/ops/{}/method", index);
			const MethodDescriptor* method = registry.Find(op.Method);
			if (method == nullptr)
			{
				std::vector<std::string> suggestions = registry.SuggestMethodNames(op.Method);
				return std::unexpected(MakeParamError(ErrorCode::NotFound, methodPointer, std::format("no method '{}'", op.Method),
					MakeDidYouMeanHint(suggestions)));
			}
			if (!method->Specification.AllowedInBatch)
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, methodPointer, std::format("{} cannot be an op of edit.batch", op.Method),
					"a batch holds reads and edits that go through the undo history (entity.*, project.setSettings, project.validate); call "
					"the others on their own"));
			}

			const Json& params = op.Params.Get();
			const std::string paramsPointer = std::format("/ops/{}/params", index);
			if (!params.is_null() && !params.is_object())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, paramsPointer, "an op's params must be an object"));
			if (params.is_object())
			{
				std::vector<ErrorIssue> issues;
				for (const std::string_view member : ReservedOpMembers)
				{
					if (params.find(std::string(member)) == params.end())
						continue;
					ErrorIssue issue;
					issue.JsonPointer = std::format("{}/{}", paramsPointer, member);
					issue.Message = std::format("'{}' is given once, on the batch, for all of its ops", member);
					issues.push_back(std::move(issue));
				}
				if (!issues.empty())
				{
					ErrorLocation location;
					location.JsonPointer = issues.front().JsonPointer;
					return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("op {} carries a reserved member", index))
							.WithHint("give dryRun and ifRevision on edit.batch itself")
							.WithLocation(std::move(location))
							.WithIssues(std::move(issues)));
				}
			}

			if (dryRun && !method->Specification.SupportsDryRun)
			{
				return std::unexpected(MakeParamError(ErrorCode::Unsupported, methodPointer, std::format("{} does not support dry runs", op.Method),
					"remove it from the dry-run batch, or run the batch for real"));
			}
			return {};
		}

		static std::vector<EntitySummary> MakeSelection(const EditorMethodContext& context)
		{
			std::vector<EntitySummary> selection;
			const EditorContext& editor = context.GetEditor();
			if (!editor.HasScene())
				return selection;
			for (const UUID id : editor.GetSelection())
			{
				const ConstEntity entity = editor.GetScene().FindEntityByID(id);
				if (entity.IsValid())
					selection.push_back(context.MakeEntitySummary(entity));
			}
			return selection;
		}

	}

	namespace Automation {

		Result<EditBatchResult> EditBatch(EditorMethodContext& context, const EditBatchParams& params)
		{
			const MethodRegistry& registry = context.GetRegistry();
			if (params.Ops.empty() || params.Ops.size() > Utils::MaxBatchOps)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/ops",
					std::format("a batch holds 1 to {} ops (got {})", Utils::MaxBatchOps, params.Ops.size())));
			}
			if (params.Label.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/label", "the batch's undo label must not be empty"));

			for (size_t index = 0; index < params.Ops.size(); ++index)
			{
				const Status checked = Utils::CheckBatchOp(registry, params.Ops[index], index, context.IsDryRun());
				if (!checked)
				{
					context.SetErrorData("failedOp", Json(index));
					return std::unexpected(checked.error());
				}
			}

			EditorTransaction transaction(context.GetEditor(), params.Label);
			std::vector<Json> results;
			results.reserve(params.Ops.size());
			for (size_t index = 0; index < params.Ops.size(); ++index)
			{
				const EditBatchOp& op = params.Ops[index];
				Json opParams = op.Params.IsNull() ? Json::object() : op.Params.Get();
				context.SetPhase(std::format("Automation:edit.batch {}/{} {}", index + 1, params.Ops.size(), op.Method));
				Status substituted = SubstituteReferences(opParams, results, 0);
				Result<Json> opResult = substituted ? registry.InvokeNested(context, op.Method, opParams) : Result<Json>(std::unexpected(substituted.error()));
				if (!opResult)
				{
					Error error = Utils::LocateOpError(opResult.error(), index);
					context.SetErrorData("failedOp", Json(index));
					const Status rolledBack = transaction.Rollback();
					if (!rolledBack)
					{
						error = Error(error).WithContext(std::format("rolling back ops 0 to {} failed: {}", index, rolledBack.error().ToString()));
						context.SetErrorData("rollbackFailed", Json(true));
					}
					return std::unexpected(std::move(error));
				}
				results.push_back(std::move(*opResult));
			}

			EditBatchResult result;
			result.UndoIndex = ToAutomationCounter(transaction.Commit());
			for (Json& opResult : results)
				result.Results.emplace_back(std::move(opResult));
			return result;
		}

		Result<EditUndoResult> EditUndo(EditorMethodContext& context, const EditUndoParams& params)
		{
			ENGINE_TRY(Utils::CheckSteps(params.Steps));
			EditorContext& editor = context.GetEditor();
			CommandHistory& history = editor.GetHistory();
			const size_t before = history.GetUndoCount();
			const Result<size_t> undone = history.Undo(editor, params.Steps);
			if (!undone)
			{
				context.SetErrorData("undone", Json(before - history.GetUndoCount()));
				return std::unexpected(undone.error());
			}

			EditUndoResult result;
			result.Undone = ToAutomationCounter(*undone);
			result.UndoIndex = ToAutomationCounter(history.GetCurrentSequence());
			result.UndoLabel = history.GetUndoLabel();
			result.RedoLabel = history.GetRedoLabel();
			return result;
		}

		Result<EditRedoResult> EditRedo(EditorMethodContext& context, const EditRedoParams& params)
		{
			ENGINE_TRY(Utils::CheckSteps(params.Steps));
			EditorContext& editor = context.GetEditor();
			CommandHistory& history = editor.GetHistory();
			const size_t before = history.GetRedoCount();
			const Result<size_t> redone = history.Redo(editor, params.Steps);
			if (!redone)
			{
				context.SetErrorData("redone", Json(before - history.GetRedoCount()));
				return std::unexpected(redone.error());
			}

			EditRedoResult result;
			result.Redone = ToAutomationCounter(*redone);
			result.UndoIndex = ToAutomationCounter(history.GetCurrentSequence());
			result.UndoLabel = history.GetUndoLabel();
			result.RedoLabel = history.GetRedoLabel();
			return result;
		}

		Result<EditHistoryResult> EditHistory(EditorMethodContext& context, const EditHistoryParams& params)
		{
			const EditorContext& editor = context.GetEditor();
			const CommandHistory& history = editor.GetHistory();
			const std::vector<CommandHistoryEntry> entries = history.GetEntries(params.Limit);
			// The entries end with the redo branch: the last GetRedoCount() of them (at most all) are undone.
			const size_t redo = std::min(entries.size(), history.GetRedoCount());

			EditHistoryResult result;
			for (size_t index = 0; index < entries.size(); ++index)
			{
				const CommandHistoryEntry& entry = entries[index];
				EditHistoryEntry item;
				item.UndoIndex = ToAutomationCounter(entry.Sequence);
				item.Label = entry.Label;
				item.Origin = entry.Origin;
				item.RevisionBefore = ToAutomationCounter(entry.RevisionBefore);
				item.RevisionAfter = ToAutomationCounter(entry.RevisionAfter);
				item.Applied = index < entries.size() - redo;
				result.Entries.push_back(std::move(item));
			}
			result.UndoIndex = ToAutomationCounter(history.GetCurrentSequence());
			result.Dirty = editor.HasScene() && editor.IsSceneDirty();
			return result;
		}

		Result<EditSelectResult> EditSelect(EditorMethodContext& context, const EditSelectParams& params)
		{
			EditorContext& editor = context.GetEditor();
			std::vector<UUID> selection;
			if (!params.Entities.empty())
			{
				ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, false));
				for (size_t index = 0; index < params.Entities.size(); ++index)
				{
					ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entities[index], std::format("/entities/{}", index)));
					selection.push_back(entity.GetUUID());
				}
			}
			// Without an open scene the selection is already empty (closing a scene clears it), so there is nothing to change.
			if (editor.HasScene())
				editor.SetSelection(std::move(selection));

			EditSelectResult result;
			result.Selection = Utils::MakeSelection(context);
			return result;
		}

		Result<EditGetSelectionResult> EditGetSelection(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditGetSelectionResult result;
			result.Selection = Utils::MakeSelection(context);
			return result;
		}

	}

	void RegisterEditMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta stepsMeta{ .Min = 1.0, .Max = static_cast<double>(Utils::MaxSteps) };

		registry.Struct<EditBatchOp>("EditBatchOp", "One op of edit.batch: a method and its params.")
			.Field("method", &EditBatchOp::Method, "The method, such as \"entity.create\"; it must be allowed in batches.")
			.Field("params", &EditBatchOp::Params,
				"The method's params (an object); {\"$ref\": \"<op index>.<path>\"} stands for a value of an earlier op's result.");

		registry.Struct<EditBatchParams>("EditBatchParams", "The params of edit.batch: ops run atomically as one undo step.")
			.Field("label", &EditBatchParams::Label, "The undo label of the batch.")
			.Field("ops", &EditBatchParams::Ops, "The ops, 1 to 1000, run in order.");

		registry.Struct<EditBatchResult>("EditBatchResult", "What every op of a batch returned.")
			.Field("results", &EditBatchResult::Results, "Each op's result, in op order.")
			.Field("undoIndex", &EditBatchResult::UndoIndex, "The batch's undo index; 0 in a dry run or when nothing changed.");

		registry.Struct<EditUndoParams>("EditUndoParams", "The params of edit.undo.")
			.Field("steps", &EditUndoParams::Steps, "How many commands to undo.", stepsMeta);

		registry.Struct<EditUndoResult>("EditUndoResult", "Where the history stands after edit.undo.")
			.Field("undone", &EditUndoResult::Undone, "Commands undone; fewer than steps when the history ran out.")
			.Field("undoIndex", &EditUndoResult::UndoIndex, "The newest applied command's undo index; 0 when none.")
			.Field("undoLabel", &EditUndoResult::UndoLabel, "What a further edit.undo would undo.")
			.Field("redoLabel", &EditUndoResult::RedoLabel, "What edit.redo would redo.");

		registry.Struct<EditRedoParams>("EditRedoParams", "The params of edit.redo.")
			.Field("steps", &EditRedoParams::Steps, "How many commands to redo.", stepsMeta);

		registry.Struct<EditRedoResult>("EditRedoResult", "Where the history stands after edit.redo.")
			.Field("redone", &EditRedoResult::Redone, "Commands redone; fewer than steps when nothing more was undone.")
			.Field("undoIndex", &EditRedoResult::UndoIndex, "The newest applied command's undo index.")
			.Field("undoLabel", &EditRedoResult::UndoLabel, "What edit.undo would undo.")
			.Field("redoLabel", &EditRedoResult::RedoLabel, "What a further edit.redo would redo.");

		registry.Struct<EditHistoryParams>("EditHistoryParams", "The params of edit.history.")
			.Field("limit", &EditHistoryParams::Limit, "The most entries returned, ending with the newest.", stepsMeta);

		registry.Struct<EditHistoryEntry>("EditHistoryEntry", "One command of the undo history.")
			.Field("undoIndex", &EditHistoryEntry::UndoIndex, "The command's undo index.")
			.Field("label", &EditHistoryEntry::Label, "The undo label, with \"[agent] \" for automation commands.")
			.Field("origin", &EditHistoryEntry::Origin, "User or Agent.")
			.Field("revisionBefore", &EditHistoryEntry::RevisionBefore, "The editor's revision before the command.")
			.Field("revisionAfter", &EditHistoryEntry::RevisionAfter, "The editor's revision after the command.")
			.Field("applied", &EditHistoryEntry::Applied, "False for undone commands (the redo branch).");

		registry.Struct<EditHistoryResult>("EditHistoryResult", "The undo history of the open scene.")
			.Field("entries", &EditHistoryResult::Entries, "The entries, oldest first, ending with the redo branch.")
			.Field("undoIndex", &EditHistoryResult::UndoIndex, "The newest applied command's undo index; 0 when none.")
			.Field("dirty", &EditHistoryResult::Dirty, "Whether the open scene has unsaved changes.");

		registry.Struct<EditSelectParams>("EditSelectParams", "The params of edit.select.")
			.Field("entities", &EditSelectParams::Entities, "The entities to select, in order; empty clears the selection.");

		registry.Struct<EditSelectResult>("EditSelectResult", "The new selection.")
			.Field("selection", &EditSelectResult::Selection, "The selected entities, in the order selected.");

		registry.Struct<EditGetSelectionResult>("EditGetSelectionResult", "The selection.")
			.Field("selection", &EditGetSelectionResult::Selection, "The selected entities, in the order selected.");
	}

	void RegisterEditMethods(MethodRegistry& methods)
	{
		Json batchExample = Json::object();
		batchExample["label"] = "Scaffold";
		Json game = Json::object();
		game["method"] = "entity.create";
		game["params"] = Json::object();
		game["params"]["name"] = "Game";
		Json board = Json::object();
		board["method"] = "entity.create";
		board["params"] = Json::object();
		board["params"]["name"] = "Board";
		board["params"]["parent"] = Json::object();
		board["params"]["parent"]["$ref"] = "0.entity.id";
		batchExample["ops"] = Json::array({ game, board });
		methods.Add(
			{
				.Name = "edit.batch",
				.Description = "Runs ops atomically as one undo step: if op k fails, ops 0 to k-1 are rolled back and the error names failedOp "
							   "k. {\"$ref\": \"<op>.<path>\"} uses an earlier op's result.",
				.RequiredParams = { "ops" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.Examples = { { .Description = "Create a game root with a board under it.", .Params = batchExample } },
			},
			&Automation::EditBatch);

		methods.Add(
			{
				.Name = "edit.undo",
				.Description = "Undoes the newest commands of the open scene's history.",
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Undo the last command.", .Params = Json::object() } },
			},
			&Automation::EditUndo);

		methods.Add(
			{
				.Name = "edit.redo",
				.Description = "Redoes the oldest undone commands of the open scene's history.",
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Redo the last undone command.", .Params = Json::object() } },
			},
			&Automation::EditRedo);

		Json historyExample = Json::object();
		historyExample["limit"] = 20;
		methods.Add(
			{
				.Name = "edit.history",
				.Description = "Lists the undo history of the open scene: labels, origins, revisions and which commands are undone.",
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the last 20 commands.", .Params = historyExample } },
			},
			&Automation::EditHistory);

		Json selectExample = Json::object();
		selectExample["entities"] = Json::array({ "/Game/Board" });
		methods.Add(
			{
				.Name = "edit.select",
				.Description = "Replaces the editor's selection (editor state, not an undoable command).",
				.RequiredParams = { "entities" },
				.Examples = { { .Description = "Select the board.", .Params = selectExample } },
			},
			&Automation::EditSelect);

		methods.Add(
			{
				.Name = "edit.getSelection",
				.Description = "Returns the editor's selection.",
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the selection.", .Params = Json::object() } },
			},
			&Automation::EditGetSelection);
	}

}
