#include "EditorPCH.h"
#include "EditorCore/Automation/SceneMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/JsonPatchDiff.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/Command.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <map>
#include <optional>

namespace Engine {

	namespace Utils {

		constexpr std::string_view SceneExtension = ".scene";

		static SceneLoadIssue MakeLoadIssue(const LoadDiagnostic& diagnostic)
		{
			SceneLoadIssue issue;
			issue.Severity = diagnostic.Severity;
			issue.Code = diagnostic.Code;
			issue.Message = diagnostic.Message;
			issue.Pointer = diagnostic.JsonPointer;
			issue.Entity = FormatOptionalUUID(diagnostic.Entity);
			return issue;
		}

		static SceneLoadRepair MakeLoadRepair(const LoadRepair& repair)
		{
			SceneLoadRepair result;
			result.Code = repair.Code;
			result.Description = repair.Description;
			result.Pointer = repair.JsonPointer;
			result.Entity = FormatOptionalUUID(repair.Entity);
			result.Removed = repair.Removed;
			return result;
		}

		// The canonical entity objects of a scene document, by id.
		static std::map<UUID, Json> IndexEntities(const Json& document)
		{
			std::map<UUID, Json> entities;
			const auto list = document.find("Entities");
			if (list == document.end() || !list->is_array())
				return entities;
			for (const Json& entity : *list)
			{
				const auto id = entity.find("ID");
				if (id == entity.end() || !id->is_string())
					continue;
				const std::optional<UUID> parsed = UUID::FromString(JsonReader(*id).ReadString().value_or(std::string()));
				if (parsed.has_value())
					entities.emplace(*parsed, entity);
			}
			return entities;
		}

		static std::string ReadEntityName(const Json& entity)
		{
			const auto name = entity.find("Name");
			return name != entity.end() && name->is_string() ? JsonReader(*name).ReadString().value_or(std::string()) : std::string();
		}

		// Per-entity RFC 6902 patches from `from` to `to` (scene documents), sorted by id (§13.7 scene.diff).
		static std::vector<SceneEntityDiff> DiffSceneDocuments(const Json& from, const Json& to)
		{
			const std::map<UUID, Json> before = IndexEntities(from);
			const std::map<UUID, Json> after = IndexEntities(to);
			std::vector<SceneEntityDiff> diffs;
			auto old = before.begin();
			auto current = after.begin();
			while (old != before.end() || current != after.end())
			{
				SceneEntityDiff diff;
				if (current == after.end() || (old != before.end() && old->first < current->first))
				{
					diff.Id = old->first.ToString();
					diff.Name = ReadEntityName(old->second);
					diff.Change = SceneEntityChangeKind::Destroyed;
					Json remove = Json::object();
					remove["op"] = "remove";
					remove["path"] = "";
					diff.Patch.emplace_back(std::move(remove));
					++old;
				}
				else if (old == before.end() || current->first < old->first)
				{
					diff.Id = current->first.ToString();
					diff.Name = ReadEntityName(current->second);
					diff.Change = SceneEntityChangeKind::Created;
					Json add = Json::object();
					add["op"] = "add";
					add["path"] = "";
					add["value"] = current->second;
					diff.Patch.emplace_back(std::move(add));
					++current;
				}
				else
				{
					const Json patch = DiffJson(old->second, current->second);
					diff.Id = current->first.ToString();
					diff.Name = ReadEntityName(current->second);
					diff.Change = SceneEntityChangeKind::Modified;
					for (const Json& operation : patch)
						diff.Patch.emplace_back(operation);
					++old;
					++current;
					if (diff.Patch.empty())
						continue;
				}
				diffs.push_back(std::move(diff));
			}
			return diffs;
		}

	}

	namespace Utils {

		// scene.new and scene.open with `save` and `discardChanges` (§13.5): with a dirty open scene exactly one of the two is
		// required, so neither or both is InvalidState; for a clean scene both together is a params error. Checked before
		// anything changes.
		static Status CheckSaveOrDiscard(const EditorContext& editor, bool save, bool discard)
		{
			if (save && discard)
			{
				if (editor.HasScene() && editor.IsSceneDirty())
				{
					return std::unexpected(Error(ErrorCode::InvalidState,
						std::format("the open scene '{}' has unsaved changes: pass exactly one of save and discardChanges, not both", editor.GetScene().GetName()))
							.WithHint("pass save: true to write it first, or discardChanges: true to drop the changes"));
				}
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/discardChanges", "give save or discardChanges, not both"));
			}
			return CheckDirtyScene(editor, save, discard, "discardChanges");
		}

		// scene.new and scene.open with keepOpen (M20, Docs/Decisions/0031-m20-contract.md decision 14): every open scene stays
		// open, so neither save nor discardChanges has anything to do, and the new scene needs a free tab. Checked before
		// anything changes.
		static Status CheckKeepOpen(const EditorContext& editor, bool save, bool discard)
		{
			if (save || discard)
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, save ? "/save" : "/discardChanges",
					"keepOpen keeps every open scene open, so there are no unsaved changes to save or discard",
					"give keepOpen alone, or drop keepOpen to replace the active scene"));
			}
			if (editor.GetOpenScenes().size() >= MaxOpenScenes)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("{} scenes are open, the most the editor holds", MaxOpenScenes))
						.WithHint("close one with scene.close first, or open the scene without keepOpen to replace the active one"));
			}
			return {};
		}

		// One open scene as scene.list and scene.activate report it.
		static OpenSceneEntry MakeOpenSceneEntry(const OpenSceneInfo& info)
		{
			OpenSceneEntry entry;
			entry.Id = info.Id;
			entry.Scene.Path = info.Path.has_value() ? ToProjectRelative(*info.Path) : std::string();
			entry.Scene.Name = info.Name;
			entry.Scene.Revision = ToAutomationCounter(info.Revision);
			entry.Scene.Dirty = info.Dirty;
			entry.Scene.EntityCount = ToAutomationCounter(info.EntityCount);
			entry.Active = info.Active;
			entry.ChangedOnDisk = info.ChangedOnDisk;
			return entry;
		}

		// InvalidState when `path` is the path of an open scene other than `replaced` (the scene the call replaces in its
		// tab, NoOpenScene when it replaces none), so no two open scenes share a path (decision 14).
		static Status CheckPathNotOpen(const EditorContext& editor, const VfsPath& path, OpenSceneId replaced, std::string_view hint)
		{
			const OpenSceneId holder = editor.FindOpenScene(path);
			if (holder == NoOpenScene || holder == replaced)
				return {};
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' is the path of another open scene", ToProjectRelative(path))).WithHint(std::string(hint)));
		}

	}

	namespace Automation {

		Result<SceneNewResult> SceneNew(EditorMethodContext& context, const SceneNewParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, context.ResolveProjectPath(params.Path, "/path", Utils::SceneExtension));
			if (params.KeepOpen)
				ENGINE_TRY(Utils::CheckKeepOpen(editor, params.Save, params.DiscardChanges));
			else
				ENGINE_TRY(Utils::CheckSaveOrDiscard(editor, params.Save, params.DiscardChanges));
			// An open scene whose file was deleted keeps its path: a new scene there would share it, unless it replaces that one.
			ENGINE_TRY(Utils::CheckPathNotOpen(editor, path, params.KeepOpen ? NoOpenScene : editor.GetActiveSceneId(),
				"close that scene with scene.close first, or choose another path"));
			if (editor.GetVfs().Exists(path))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path", std::format("'{}' already exists", params.Path),
					"open it with scene.open, or choose another path"));
			}

			Scope<Scene> scene = editor.CreateScene(std::string(path.GetStem()));
			ENGINE_TRY_ASSIGN(const std::string text, SceneSerializer::SaveToString(*scene));
			SceneNewResult result;
			if (params.KeepOpen)
			{
				ENGINE_TRY(editor.WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
				ENGINE_TRY_ASSIGN(result.Id, editor.AddScene(std::move(scene), path, false));
			}
			else
			{
				ENGINE_TRY(Utils::ResolveDirtyScene(editor, params.Save, params.DiscardChanges, "discardChanges"));
				ENGINE_TRY(editor.WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
				editor.SetScene(std::move(scene), path, false);
				result.Id = editor.GetActiveSceneId();
			}

			result.Scene = Utils::MakeSceneSummary(editor);
			return result;
		}

		Result<SceneOpenResult> SceneOpen(EditorMethodContext& context, const SceneOpenParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, context.ResolveProjectPath(params.Path, "/path", Utils::SceneExtension));
			if (params.KeepOpen && params.Reload)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/reload",
					"reload reads an open scene again in its own tab and keepOpen opens a scene in a new one: give one of them"));
			}
			// The path of an open scene (M20, decision 14): the active scene's, read again in place as before M20, or another
			// tab's, read again in that tab, which then becomes active. Either needs reload.
			const bool isActivePath = editor.HasScene() && editor.GetScenePath().has_value() && *editor.GetScenePath() == path;
			const OpenSceneId otherId = isActivePath ? NoOpenScene : editor.FindOpenScene(path);
			if ((isActivePath || otherId != NoOpenScene) && !params.Reload)
			{
				return std::unexpected(Error(ErrorCode::InvalidState,
					std::format("'{}' is already open{}", params.Path, isActivePath ? " as the active scene" : ""))
						.WithHint(isActivePath ? "pass reload: true to read it again from disk"
											   : "make it the active scene with scene.activate {scene: <path>}, or pass reload: true to read it again from disk"));
			}
			if (params.KeepOpen)
			{
				ENGINE_TRY(Utils::CheckKeepOpen(editor, params.Save, params.DiscardChanges));
			}
			else if (otherId != NoOpenScene)
			{
				// The tab read again keeps the active scene as it is, so the save-or-discard rule is that tab's; it is saved
				// first because its file is about to be read.
				ENGINE_TRY_ASSIGN(const Scope<EditorSceneTargetScope> reloaded, EditorSceneTargetScope::Begin(editor, otherId));
				ENGINE_TRY(Utils::CheckSaveOrDiscard(editor, params.Save, params.DiscardChanges));
				if (params.Save)
					ENGINE_TRY(Utils::ResolveDirtyScene(editor, true, false, "discardChanges"));
			}
			else
			{
				ENGINE_TRY(Utils::CheckSaveOrDiscard(editor, params.Save, params.DiscardChanges));
			}

			// Saving the open scene to the path about to be read must happen first; otherwise the new scene is loaded before
			// anything changes, so a file that fails to load leaves the editor exactly as it was.
			if (isActivePath && params.Save)
				ENGINE_TRY(Utils::ResolveDirtyScene(editor, true, false, "discardChanges"));

			// The registry first (§7.3: a path-taking call refreshes first), so the instances below are rebuilt from prefabs as
			// they are on disk now.
			context.SetPhase(std::format("Automation:scene.open {}", params.Path));
			ENGINE_TRY(Utils::RefreshAssets(editor));

			Scope<Scene> scene = editor.CreateScene(std::string(path.GetStem()));
			LoadReport report;
			ENGINE_TRY(Utils::LoadSceneFile(editor, *scene, path, params.Repair ? LoadMode::Repair : LoadMode::Strict, &editor.GetIdGenerator(), report));

			// §5.5 "Update": the instances follow the current version of their prefabs, so a scene reloaded after a
			// SceneChangedOnDisk also adopts the prefabs that changed; the scene then opens dirty. An update that fails (a prefab
			// that no longer imports) leaves the instances as saved, with a warning, rather than refusing to open the scene.
			bool dirty = params.Repair && !report.Repairs.empty();
			if (Result<bool> updated = editor.UpdatePrefabInstances(*scene, {}); updated)
			{
				dirty = dirty || *updated;
			}
			else
			{
				ENGINE_WARN("'{}': the prefab instances keep their saved state because they could not be updated: {}", params.Path,
					updated.error().ToString());
			}
			SceneOpenResult result;
			if (params.KeepOpen)
			{
				Utils::LogLoadDiagnostics(Utils::ToProjectRelative(path), report);
				ENGINE_TRY_ASSIGN(result.Id, editor.AddScene(std::move(scene), path, dirty));
			}
			else
			{
				if (otherId != NoOpenScene)
					ENGINE_TRY(editor.ActivateScene(otherId));
				else if (!isActivePath)
					ENGINE_TRY(Utils::ResolveDirtyScene(editor, params.Save, params.DiscardChanges, "discardChanges"));
				Utils::LogLoadDiagnostics(Utils::ToProjectRelative(path), report);
				editor.SetScene(std::move(scene), path, dirty);
				result.Id = editor.GetActiveSceneId();
			}

			result.Scene = Utils::MakeSceneSummary(editor);
			result.Migrated = report.Migrated;
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
				result.Diagnostics.push_back(Utils::MakeLoadIssue(diagnostic));
			for (const LoadRepair& repair : report.Repairs)
				result.Repairs.push_back(Utils::MakeLoadRepair(repair));
			return result;
		}

		Result<SceneSaveResult> SceneSave(EditorMethodContext& context, const SceneSaveParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (!editor.HasScene())
				return std::unexpected(Error(ErrorCode::InvalidState, "no scene open").WithHint("create one with scene.new or open one with scene.open"));

			VfsPath path;
			if (params.Path.empty())
			{
				ENGINE_TRY_ASSIGN(path, Utils::GetOwnScenePath(editor));
			}
			else
			{
				ENGINE_TRY_ASSIGN(path, context.ResolveProjectPath(params.Path, "/path", Utils::SceneExtension));
				// M20 (decision 14): no two open scenes share a path.
				ENGINE_TRY(Utils::CheckPathNotOpen(editor, path, editor.GetAddressedSceneId(),
					"close that scene with scene.close {scene: <path>} first, or save to another path"));
			}
			ENGINE_TRY(Utils::SaveOpenScene(editor, path));

			SceneSaveResult result;
			result.Scene = Utils::MakeSceneSummary(editor);
			result.File = Utils::ToProjectRelative(path);
			return result;
		}

		Result<SceneDiffResult> SceneDiff(EditorMethodContext& context, const SceneDiffParams& params)
		{
			EditorContext& editor = context.GetEditor();
			if (!editor.HasScene())
				return std::unexpected(Error(ErrorCode::InvalidState, "no scene open").WithHint("open one with scene.open"));
			const uint64_t currentRevision = editor.GetRevision();
			ENGINE_TRY_ASSIGN(const Json current, SceneSerializer::ToJson(editor.GetScene()));

			SceneDiffResult result;
			result.ToRevision = ToAutomationCounter(currentRevision);
			if (params.Against == SceneDiffAgainst::Saved)
			{
				if (!editor.GetScenePath().has_value())
				{
					return std::unexpected(Error(ErrorCode::InvalidState, "the open scene was never saved, so there is no saved version to compare with")
							.WithHint("save it with scene.save {path}, or compare with a revision"));
				}
				// The saved file through the serializer (Repair, on a scratch scene and generator), so its document is canonical
				// and in the current format, like the open scene's.
				UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
				SceneSpecification specification;
				specification.Name = editor.GetScene().GetName();
				specification.Registry = &editor.GetTypeRegistry();
				specification.IdGenerator = &scratchIds;
				const Scope<Scene> saved = Scene::Create(specification);
				LoadReport report;
				ENGINE_TRY(Utils::LoadSceneFile(editor, *saved, *editor.GetScenePath(), LoadMode::Repair, &scratchIds, report));
				ENGINE_TRY_ASSIGN(const Json savedDocument, SceneSerializer::ToJson(*saved));
				result.Entities = Utils::DiffSceneDocuments(savedDocument, current);
				return result;
			}

			if (params.Revision == ToAutomationCounter(currentRevision))
			{
				result.FromRevision = params.Revision;
				return result;
			}
			// An earlier revision is rebuilt by replaying the history on a scratch copy (ADR 0008 decisions 13 and 29): the entries
			// are oldest first and the first GetUndoCount() of them are applied, so the target state is reached by reverting
			// the applied entries after it (newest first) or by replaying the undone entries before it (oldest first).
			const CommandHistory& history = editor.GetHistory();
			const std::vector<CommandHistoryEntry> entries = history.GetEntries(std::numeric_limits<size_t>::max());
			std::optional<size_t> target; // the number of entries applied in the target state
			for (size_t index = 0; index < entries.size() && !target.has_value(); ++index)
			{
				if (ToAutomationCounter(entries[index].RevisionBefore) == params.Revision)
					target = index;
				else if (ToAutomationCounter(entries[index].RevisionAfter) == params.Revision)
					target = index + 1;
			}
			if (!target.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::NotFound, "/revision",
					std::format("revision {} is not held by the undo history (the current revision is {})", params.Revision, currentRevision),
					"compare with against: \"saved\", or with a revision edit.history lists"));
			}

			// The copy gets a scratch id generator, so reading never advances the editor's own ids.
			UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
			SceneSpecification specification;
			specification.Name = editor.GetScene().GetName();
			specification.Registry = &editor.GetTypeRegistry();
			specification.IdGenerator = &scratchIds;
			const Scope<Scene> earlier = Scene::Create(specification);
			LoadReport report;
			ENGINE_TRY(SceneSerializer::FromJson(*earlier, current, LoadOptions{}, report));
			const size_t applied = history.GetUndoCount();
			const bool forward = *target > applied;
			const size_t steps = forward ? *target - applied : applied - *target;
			for (size_t step = 0; step < steps; ++step)
			{
				const size_t index = forward ? applied + step : applied - 1 - step;
				const Command* command = history.FindCommand(entries[index].Sequence);
				ENGINE_ASSERT(command != nullptr, "the history entry {} has no command", entries[index].Sequence);
				if (command == nullptr)
					return MakeError(ErrorCode::InvalidState, "the history entry {} has no command", entries[index].Sequence);
				if (Status replayed = command->ReplayOnSceneCopy(*earlier, forward); !replayed)
					return std::unexpected(std::move(replayed).error().WithContext(std::format("rebuilding revision {}", params.Revision)));
			}
			ENGINE_TRY_ASSIGN(const Json earlierDocument, SceneSerializer::ToJson(*earlier));
			result.FromRevision = params.Revision;
			result.Entities = Utils::DiffSceneDocuments(earlierDocument, current);
			return result;
		}

		Result<SceneListResult> SceneList(EditorMethodContext& context, const NoParams& /*params*/)
		{
			const EditorContext& editor = context.GetEditor();
			SceneListResult result;
			for (const OpenSceneId id : editor.GetOpenScenes())
			{
				ENGINE_TRY_ASSIGN(const OpenSceneInfo info, editor.DescribeOpenScene(id));
				result.Scenes.push_back(Utils::MakeOpenSceneEntry(info));
			}
			return result;
		}

		Result<SceneActivateResult> SceneActivate(EditorMethodContext& context, const NoParams& /*params*/)
		{
			// The target is the subject (RequiresSceneTarget), so PrepareParams has refused a request without one.
			if (!context.GetOptions().Scene.has_value())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/scene", "scene.activate needs the scene to activate"));
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const OpenSceneId id, context.ResolveSceneTarget());
			ENGINE_TRY(editor.ActivateScene(id));
			ENGINE_TRY_ASSIGN(const OpenSceneInfo info, editor.DescribeOpenScene(id));
			SceneActivateResult result;
			result.Scene = Utils::MakeOpenSceneEntry(info);
			return result;
		}

		Result<SceneCloseResult> SceneClose(EditorMethodContext& context, const SceneCloseParams& params)
		{
			if (params.Save && params.DiscardChanges)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/discardChanges", "give save or discardChanges, not both"));
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const OpenSceneId id, context.ResolveSceneTarget());
			// The automation server addresses a request's scene target from its admission on; a caller that does not (an
			// editor UI action closing a tab) is given that scope here, so the checks, the save and the summary below
			// address the scene being closed.
			Scope<EditorSceneTargetScope> addressed;
			if (id != editor.GetAddressedSceneId())
			{
				ENGINE_TRY_ASSIGN(addressed, EditorSceneTargetScope::Begin(editor, id));
			}
			ENGINE_TRY(Utils::CheckDirtyScene(editor, params.Save, params.DiscardChanges, "discardChanges"));

			SceneCloseResult result;
			if (params.Save && editor.IsSceneDirty())
			{
				ENGINE_TRY_ASSIGN(const VfsPath path, Utils::GetOwnScenePath(editor));
				ENGINE_TRY(Utils::SaveOpenScene(editor, path));
				result.File = Utils::ToProjectRelative(path);
			}
			result.Closed = Utils::MakeSceneSummary(editor);
			ENGINE_TRY(editor.CloseScene(id));
			result.ActiveId = editor.GetActiveSceneId();
			return result;
		}

	}

	void RegisterEditorSceneMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<SceneTemplate>("SceneTemplate", "What a new scene starts with.")
			.Entry(SceneTemplate::Empty, "Empty", "No entities.");

		registry.Enum<SceneDiffAgainst>("SceneDiffAgainst", "What scene.diff compares the open scene with.")
			.Entry(SceneDiffAgainst::Saved, "Saved", "The scene's file on disk.")
			.Entry(SceneDiffAgainst::Revision, "Revision", "The scene as it was at an earlier revision the undo history holds.");

		registry.Enum<SceneEntityChangeKind>("SceneEntityChangeKind", "How one entity changed.")
			.Entry(SceneEntityChangeKind::Created, "Created", "The entity did not exist before.")
			.Entry(SceneEntityChangeKind::Destroyed, "Destroyed", "The entity no longer exists.")
			.Entry(SceneEntityChangeKind::Modified, "Modified", "The entity exists in both and differs.");

		registry.Struct<SceneLoadIssue>("SceneLoadIssue", "A warning or error found while loading a scene file.")
			.Field("severity", &SceneLoadIssue::Severity, "Warning or Error.")
			.Field("code", &SceneLoadIssue::Code, "The load code, such as \"SCENE_UNKNOWN_COMPONENT\".")
			.Field("message", &SceneLoadIssue::Message, "What was found.")
			.Field("pointer", &SceneLoadIssue::Pointer, "The JSON pointer within the file.")
			.Field("entity", &SceneLoadIssue::Entity, "The id of the entity, as written in the file; empty when not about one entity.");

		registry.Struct<SceneLoadRepair>("SceneLoadRepair", "A repair a repair load applied.")
			.Field("code", &SceneLoadRepair::Code, "The code of the defect it fixed.")
			.Field("description", &SceneLoadRepair::Description, "What it did.")
			.Field("pointer", &SceneLoadRepair::Pointer, "The JSON pointer within the file.")
			.Field("entity", &SceneLoadRepair::Entity, "The id of the entity it changed; empty when none.")
			.Field("removed", &SceneLoadRepair::Removed, "The JSON of a component it dropped or reset; null otherwise.");

		registry.Struct<SceneNewParams>("SceneNewParams", "The params of scene.new.")
			.Field("path", &SceneNewParams::Path, "The new scene's project-relative path, ending with \".scene\".")
			.Field("template", &SceneNewParams::Template, "What the scene starts with.")
			.Field("save", &SceneNewParams::Save, "Save the active scene, which the new one replaces, first when it has unsaved changes.")
			.Field("discardChanges", &SceneNewParams::DiscardChanges, "Drop the unsaved changes of the active scene, which the new one replaces.")
			.Field("keepOpen", &SceneNewParams::KeepOpen, "Keep every open scene open and add this one as the active scene.");

		registry.Struct<SceneNewResult>("SceneNewResult", "The new open scene.")
			.Field("scene", &SceneNewResult::Scene, "The new scene.")
			.Field("undoIndex", &SceneNewResult::UndoIndex, "Always 0: opening a scene starts a new history.")
			.Field("id", &SceneNewResult::Id, "The scene's open-scene id, which the scene target accepts.");

		registry.Struct<SceneOpenParams>("SceneOpenParams", "The params of scene.open.")
			.Field("path", &SceneOpenParams::Path, "The scene's project-relative path.")
			.Field("save", &SceneOpenParams::Save, "Save the scene this one replaces (the active one, or the open scene reload reads again) first when it has unsaved changes.")
			.Field("discardChanges", &SceneOpenParams::DiscardChanges, "Drop the unsaved changes of the scene this one replaces.")
			.Field("reload", &SceneOpenParams::Reload, "Read an open scene's file again in its tab (a version changed on disk) and make it the active scene.")
			.Field("repair", &SceneOpenParams::Repair, "Load with structural repairs and report them; the scene is then unsaved.")
			.Field("keepOpen", &SceneOpenParams::KeepOpen, "Keep every open scene open and add this one as the active scene.");

		registry.Struct<SceneOpenResult>("SceneOpenResult", "The opened scene and what loading it reported.")
			.Field("scene", &SceneOpenResult::Scene, "The open scene.")
			.Field("migrated", &SceneOpenResult::Migrated, "The file was in an older format; saving it, or project.upgrade, rewrites it.")
			.Field("diagnostics", &SceneOpenResult::Diagnostics, "The load warnings, also logged.")
			.Field("repairs", &SceneOpenResult::Repairs, "The repairs a repair load applied.")
			.Field("id", &SceneOpenResult::Id, "The scene's open-scene id, which the scene target accepts.");

		registry.Struct<SceneSaveParams>("SceneSaveParams", "The params of scene.save.")
			.Field("path", &SceneSaveParams::Path, "Where to save, project-relative; empty for the scene's own file.");

		registry.Struct<SceneSaveResult>("SceneSaveResult", "The saved scene.")
			.Field("scene", &SceneSaveResult::Scene, "The open scene after saving.")
			.Field("file", &SceneSaveResult::File, "The project-relative file written.");

		registry.Struct<SceneDiffParams>("SceneDiffParams", "The params of scene.diff.")
			.Field("against", &SceneDiffParams::Against, "Compare with the saved file or with an earlier revision.")
			.Field("revision", &SceneDiffParams::Revision, "The revision to compare with (against revision).");

		registry.Struct<SceneEntityDiff>("SceneEntityDiff", "What changed on one entity.")
			.Field("id", &SceneEntityDiff::Id, "The entity's id.")
			.Field("name", &SceneEntityDiff::Name, "The entity's current name (its old one when destroyed).")
			.Field("change", &SceneEntityDiff::Change, "Created, Destroyed or Modified.")
			.Field("patch", &SceneEntityDiff::Patch, "The RFC 6902 operations from the old to the current entity JSON.");

		registry.Struct<SceneDiffResult>("SceneDiffResult", "The differences between the open scene and an earlier version.")
			.Field("fromRevision", &SceneDiffResult::FromRevision, "The compared revision; 0 for the saved file.")
			.Field("toRevision", &SceneDiffResult::ToRevision, "The current revision.")
			.Field("entities", &SceneDiffResult::Entities, "The changed entities, sorted by id.");

		registry.Struct<OpenSceneEntry>("OpenSceneEntry", "One open scene as scene.list reports it.")
			.Field("id", &OpenSceneEntry::Id, "Its open-scene id, which the scene target accepts.")
			.Field("scene", &OpenSceneEntry::Scene, "Its path (empty for a scene never saved), name, revision, dirty flag and entity count.")
			.Field("active", &OpenSceneEntry::Active, "Whether it is the active scene.")
			.Field("changedOnDisk", &OpenSceneEntry::ChangedOnDisk, "Its file or a prefab it instantiates changed on disk (SceneChangedOnDisk).");

		registry.Struct<SceneListResult>("SceneListResult", "The editor's open scenes.")
			.Field("scenes", &SceneListResult::Scenes, "The open scenes in tab order; empty without an open scene.");

		registry.Struct<SceneActivateResult>("SceneActivateResult", "The scene scene.activate made active.")
			.Field("scene", &SceneActivateResult::Scene, "The now active scene.");

		registry.Struct<SceneCloseParams>("SceneCloseParams", "The params of scene.close.")
			.Field("save", &SceneCloseParams::Save, "Write the scene to its file first when it has unsaved changes.")
			.Field("discardChanges", &SceneCloseParams::DiscardChanges, "Drop the scene's unsaved changes.");

		registry.Struct<SceneCloseResult>("SceneCloseResult", "The closed scene and the now active one.")
			.Field("closed", &SceneCloseResult::Closed, "The closed scene as it was when it closed (after the save, when save wrote it).")
			.Field("file", &SceneCloseResult::File, "The project-relative file save wrote; empty when nothing was written.")
			.Field("activeId", &SceneCloseResult::ActiveId, "The open-scene id of the now active scene; 0 when no scene is open.");
	}

	void RegisterEditorSceneMethods(MethodRegistry& methods)
	{
		Json newExample = Json::object();
		newExample["path"] = "Assets/Scenes/Main.scene";
		methods.Add(
			{
				.Name = "scene.new",
				.Description = "Creates a scene file at path and opens it in place of the active scene, which needs save or discardChanges when it "
							   "has unsaved changes; keepOpen adds it as another open scene instead.",
				.RequiredParams = { "path" },
				.ExposeAsTool = true,
				.Mutates = true,
				.Examples = { { .Description = "Create the main scene.", .Params = newExample } },
			},
			&Automation::SceneNew);

		Json openExample = Json::object();
		openExample["path"] = "Assets/Scenes/Level1.scene";
		openExample["save"] = true;
		methods.Add(
			{
				.Name = "scene.open",
				.Description = "Opens the scene at path in place of the active scene, which needs save or discardChanges when it has unsaved "
							   "changes; keepOpen adds it as another open scene instead. reload reads an open scene again; repair loads a damaged "
							   "file with reported repairs.",
				.RequiredParams = { "path" },
				.ExposeAsTool = true,
				.Examples = { { .Description = "Save the open scene, then open Level1.", .Params = openExample } },
			},
			&Automation::SceneOpen);

		methods.Add(
			{
				.Name = "scene.save",
				.Description = "Writes the open scene to its file, or to path (which then becomes its file), and records the write in "
							   "provenance.",
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsSceneTarget = true,
				.Examples = { { .Description = "Save the open scene.", .Params = Json::object() } },
			},
			&Automation::SceneSave);

		Json diffExample = Json::object();
		diffExample["against"] = "Saved";
		methods.Add(
			{
				.Name = "scene.diff",
				.Description = "Reports what changed per entity, as RFC 6902 patches, against the saved file or an earlier revision.",
				.RequiredParams = { "against" },
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Review the unsaved changes.", .Params = diffExample } },
			},
			&Automation::SceneDiff);

		methods.Add(
			{
				.Name = "scene.list",
				.Description = "Lists the open scenes in tab order, with the active one marked.",
				.ExposeAsTool = true,
				.Examples = { { .Description = "List the open scenes.", .Params = Json::object() } },
			},
			&Automation::SceneList);

		Json activateExample = Json::object();
		activateExample["scene"] = "Assets/Scenes/Level1.scene";
		methods.Add(
			{
				.Name = "scene.activate",
				.Description = "Makes the open scene the scene target names the active scene.",
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.RequiresSceneTarget = true,
				.Examples = { { .Description = "Switch to the open Level1 scene.", .Params = activateExample } },
			},
			&Automation::SceneActivate);

		Json closeExample = Json::object();
		closeExample["save"] = true;
		methods.Add(
			{
				.Name = "scene.close",
				.Description = "Closes an open scene; with unsaved changes, save or discardChanges is required.",
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.Examples = { { .Description = "Save the active scene, then close it.", .Params = closeExample } },
			},
			&Automation::SceneClose);
	}

}
