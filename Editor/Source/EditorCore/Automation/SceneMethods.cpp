#include "EditorPCH.h"
#include "EditorCore/Automation/SceneMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/JsonPatchDiff.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/Command.h"
#include "EditorCore/Commands/CommandHistory.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <limits>
#include <map>
#include <optional>

namespace Engine {

	namespace {

		// What scene.tree visits: entities below the roots up to the depth limit.
		struct TreeWalk
		{
			const Scene* Target = nullptr;
			uint32_t DepthLimit = 0; // 0: unlimited
			std::vector<SceneTreeEntry>* Entries = nullptr;
			std::string* Text = nullptr;
		};

	}

	namespace Utils {

		constexpr std::string_view SceneExtension = ".scene";
		// Text shown for a component in the scene.tree outline is cut to this many characters.
		constexpr size_t MaxOutlineTextLength = 24;

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

		// A float as the outline prints it: the shortest text that reads back as the same float ("4.5", "20").
		static std::string FormatOutlineNumber(float value)
		{
			return std::format("{}", value == 0.0f ? 0.0f : value);
		}

		// Text for the outline: quotes, cut to MaxOutlineTextLength characters (at a UTF-8 boundary) with an ellipsis.
		static std::string FormatOutlineText(std::string_view text)
		{
			if (text.size() <= MaxOutlineTextLength)
				return std::format("\"{}\"", text);
			size_t cut = MaxOutlineTextLength;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80)
				--cut;
			return std::format("\"{}…\"", text.substr(0, cut));
		}

		// The short form of one component in the outline (§13.7): "Camera(Ortho 11)", "Script(<8 hex digits>)",
		// "Text(\"Score 0\")", or its name.
		static std::string FormatOutlineComponent(ConstEntity entity, const ComponentInfo& info)
		{
			if (info.GetName() == "Camera")
			{
				if (const CameraComponent* camera = entity.TryGetComponent<CameraComponent>())
				{
					return camera->Projection == ProjectionType::Orthographic ? std::format("Camera(Ortho {})", FormatOutlineNumber(camera->OrthographicSize))
																			  : std::format("Camera(Persp {})", FormatOutlineNumber(camera->VerticalFov));
				}
			}
			if (info.GetName() == "Script" || info.GetName() == "Text")
			{
				const Result<Json> json = ComponentAccess::GetComponentJson(entity, info.GetName());
				const auto member = json.has_value() ? json->find(info.GetName()) : Json::const_iterator();
				if (json.has_value() && member != json->end() && member->is_string())
				{
					const std::string text = JsonReader(*member).ReadString().value_or(std::string());
					if (info.GetName() == "Text")
						return std::format("Text({})", FormatOutlineText(text));
					return std::format("Script({})", text.substr(0, 8));
				}
			}
			return info.GetName();
		}

		static std::string FormatOutlineTranslation(ConstEntity entity)
		{
			const TransformComponent* transform = entity.TryGetComponent<TransformComponent>();
			if (transform == nullptr)
				return {};
			const glm::vec3& translation = transform->Translation;
			return std::format("({}, {}, {})", FormatOutlineNumber(translation.x), FormatOutlineNumber(translation.y), FormatOutlineNumber(translation.z));
		}

		static std::string FormatCount(size_t count, std::string_view singular, std::string_view plural)
		{
			return std::format("{} {}", count, count == 1 ? singular : plural);
		}

		// Appends `root` and its subtree to the walk, depth-first in canonical order: one entry per entity, or one outline line
		// with "├─ "/"└─ " branches (none for a `top` root, whose children start at the margin). Children beyond the depth limit
		// become a "… <n> children (depth limit)" line. Iterative, because hierarchies may be deep.
		static void WalkTree(const TreeWalk& walk, ConstEntity root, bool last, bool top)
		{
			struct Pending
			{
				ConstEntity Node{};
				uint32_t Depth = 0;
				std::string Prefix{};
				bool Last = true;
				bool Top = false;
			};

			const Scene& scene = *walk.Target;
			std::vector<Pending> stack;
			stack.push_back(Pending{ root, 0, std::string(), last, top });
			while (!stack.empty())
			{
				const Pending current = std::move(stack.back());
				stack.pop_back();
				const ConstEntity entity = current.Node;
				const std::span<const UUID> children = entity.GetChildren();
				const bool cut = walk.DepthLimit != 0 && current.Depth + 1 >= walk.DepthLimit && !children.empty();

				if (walk.Entries != nullptr)
				{
					SceneTreeEntry entry;
					entry.Id = entity.GetUUID().ToString();
					entry.Name = entity.GetName();
					entry.Path = scene.GetEntityPath(entity);
					entry.Parent = current.Depth == 0 || !entity.GetParent().IsValid() ? std::string() : entity.GetParent().GetUUID().ToString();
					entry.Depth = current.Depth;
					entry.Active = entity.IsActiveSelf();
					for (const ComponentInfo* info : GetEntityComponents(entity))
						entry.Components.push_back(info->GetName());
					entry.ChildCount = ToAutomationCounter(children.size());
					entry.ChildrenOmitted = cut;
					walk.Entries->push_back(std::move(entry));
				}

				const std::string childPrefix = current.Top ? std::string() : current.Prefix + (current.Last ? "   " : "│  ");
				if (walk.Text != nullptr)
				{
					const std::string branch = current.Top ? std::string() : current.Prefix + (current.Last ? "└─ " : "├─ ");
					std::string components;
					for (const ComponentInfo* info : GetEntityComponents(entity))
						components += (components.empty() ? "" : " ") + FormatOutlineComponent(entity, *info);
					*walk.Text += std::format("{}{}  {}  {}  {}\n", branch, entity.GetName(), entity.GetUUID().ToString().substr(0, 8), components,
						FormatOutlineTranslation(entity));
					if (cut)
						*walk.Text += std::format("{}└─ … {} (depth limit)\n", childPrefix, FormatCount(children.size(), "child", "children"));
				}
				if (cut)
					continue;

				// Pushed last child first, so the first child is visited next.
				for (size_t index = children.size(); index > 0; --index)
				{
					const ConstEntity child = scene.FindEntityByID(children[index - 1]);
					if (child.IsValid())
						stack.push_back(Pending{ child, current.Depth + 1, childPrefix, index == children.size(), false });
				}
			}
		}

		// Whether `name` matches the scene.query pattern: '*' any run of characters, '?' one character, everything else
		// itself (case-sensitive). Iterative, with backtracking to the last '*'.
		static bool MatchesNamePattern(std::string_view name, std::string_view pattern)
		{
			size_t n = 0;
			size_t p = 0;
			size_t starPattern = std::string_view::npos;
			size_t starName = 0;
			while (n < name.size())
			{
				if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == name[n]))
				{
					++n;
					++p;
				}
				else if (p < pattern.size() && pattern[p] == '*')
				{
					starPattern = p++;
					starName = n;
				}
				else if (starPattern != std::string_view::npos)
				{
					p = starPattern + 1;
					n = ++starName;
				}
				else
				{
					return false;
				}
			}
			while (p < pattern.size() && pattern[p] == '*')
				++p;
			return p == pattern.size();
		}

		// A component name of scene.query, checked against the registry. Errors: InvalidArgument at `pointer` for an unknown,
		// entity-level or unserialized component (with "did you mean" suggestions).
		static Result<const ComponentInfo*> FindQueryComponent(const TypeRegistry& registry, std::string_view name, std::string_view pointer)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			if (info == nullptr || !info->HasFlag(ComponentFlags::Serializable))
			{
				std::vector<std::string> suggestions = registry.SuggestComponentNames(name);
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("no component '{}'", name), MakeDidYouMeanHint(suggestions)));
			}
			if (info->HasFlag(ComponentFlags::EntityLevel))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is an entity member, not a component", name), "query names with where.name and tags with where.tag"));
			}
			return info;
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

	}

	namespace Automation {

		Result<SceneNewResult> SceneNew(EditorMethodContext& context, const SceneNewParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, context.ResolveProjectPath(params.Path, "/path", Utils::SceneExtension));
			ENGINE_TRY(Utils::CheckSaveOrDiscard(editor, params.Save, params.DiscardChanges));
			if (editor.GetVfs().Exists(path))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::AlreadyExists, "/path", std::format("'{}' already exists", params.Path),
					"open it with scene.open, or choose another path"));
			}

			Scope<Scene> scene = editor.CreateScene(std::string(path.GetStem()));
			ENGINE_TRY_ASSIGN(const std::string text, SceneSerializer::SaveToString(*scene));
			ENGINE_TRY(Utils::ResolveDirtyScene(editor, params.Save, params.DiscardChanges, "discardChanges"));
			ENGINE_TRY(editor.WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
			editor.SetScene(std::move(scene), path, false);

			SceneNewResult result;
			result.Scene = Utils::MakeSceneSummary(editor);
			return result;
		}

		Result<SceneOpenResult> SceneOpen(EditorMethodContext& context, const SceneOpenParams& params)
		{
			EditorContext& editor = context.GetEditor();
			ENGINE_TRY_ASSIGN(const VfsPath path, context.ResolveProjectPath(params.Path, "/path", Utils::SceneExtension));
			const bool isOpenPath = editor.HasScene() && editor.GetScenePath().has_value() && *editor.GetScenePath() == path;
			if (isOpenPath && !params.Reload)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' is already the open scene", params.Path))
						.WithHint("pass reload: true to read it again from disk"));
			}
			ENGINE_TRY(Utils::CheckSaveOrDiscard(editor, params.Save, params.DiscardChanges));

			// Saving the open scene to the path about to be read must happen first; otherwise the new scene is loaded before
			// anything changes, so a file that fails to load leaves the editor exactly as it was.
			if (isOpenPath && params.Save)
				ENGINE_TRY(Utils::ResolveDirtyScene(editor, true, false, "discardChanges"));

			Scope<Scene> scene = editor.CreateScene(std::string(path.GetStem()));
			LoadReport report;
			context.SetPhase(std::format("Automation:scene.open {}", params.Path));
			ENGINE_TRY(Utils::LoadSceneFile(editor, *scene, path, params.Repair ? LoadMode::Repair : LoadMode::Strict, &editor.GetIdGenerator(), report));
			if (!isOpenPath)
				ENGINE_TRY(Utils::ResolveDirtyScene(editor, params.Save, params.DiscardChanges, "discardChanges"));

			Utils::LogLoadDiagnostics(Utils::ToProjectRelative(path), report);
			editor.SetScene(std::move(scene), path, params.Repair && !report.Repairs.empty());

			SceneOpenResult result;
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
			}
			ENGINE_TRY(Utils::SaveOpenScene(editor, path));

			SceneSaveResult result;
			result.Scene = Utils::MakeSceneSummary(editor);
			result.File = Utils::ToProjectRelative(path);
			return result;
		}

		Result<SceneTreeResult> SceneTree(EditorMethodContext& context, const SceneTreeParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			const EditorContext& editor = context.GetEditor();

			std::vector<ConstEntity> roots;
			if (params.Root.empty())
			{
				for (const UUID id : scene->GetRootEntities())
					roots.push_back(std::as_const(*scene).FindEntityByID(id));
			}
			else
			{
				ENGINE_TRY_ASSIGN(const Entity root, context.ResolveEntity(*scene, params.Root, "/root"));
				roots.push_back(root);
			}

			SceneTreeResult result;
			result.Scene = Utils::MakeSceneSummary(editor);
			TreeWalk walk;
			walk.Target = scene;
			walk.DepthLimit = params.Depth;
			if (params.Format == SceneTreeFormat::Json)
			{
				walk.Entries = &result.Entities;
			}
			else
			{
				walk.Text = &result.Text;
				const std::string file = editor.GetScenePath().has_value() ? std::string(editor.GetScenePath()->GetFileName())
																		   : std::format("{} (unsaved)", scene->GetName());
				result.Text = std::format("{}  rev {}  {}{}\n", file, editor.GetRevision(), editor.IsSceneDirty() ? "dirty  " : "",
					Utils::FormatCount(scene->GetEntityCount(), "entity", "entities"));
			}
			for (size_t index = 0; index < roots.size(); ++index)
			{
				if (roots[index].IsValid())
					Utils::WalkTree(walk, roots[index], index + 1 == roots.size(), !params.Root.empty());
			}
			return result;
		}

		Result<SceneQueryResult> SceneQuery(EditorMethodContext& context, const SceneQueryParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			const TypeRegistry& registry = scene->GetTypeRegistry();

			const ComponentInfo* required = nullptr;
			if (!params.Where.Component.empty())
			{
				ENGINE_TRY_ASSIGN(required, Utils::FindQueryComponent(registry, params.Where.Component, "/where/component"));
			}

			bool selectAll = false;
			std::vector<const ComponentInfo*> selected;
			for (size_t index = 0; index < params.Select.size(); ++index)
			{
				if (params.Select[index] == "all")
				{
					selectAll = true;
					continue;
				}
				ENGINE_TRY_ASSIGN(const ComponentInfo* info, Utils::FindQueryComponent(registry, params.Select[index], std::format("/select/{}", index)));
				selected.push_back(info);
			}

			std::optional<UUID> under;
			if (!params.Where.Path.empty())
			{
				ENGINE_TRY_ASSIGN(const Entity root, context.ResolveEntity(*scene, params.Where.Path, "/where/path"));
				under = root.GetUUID();
			}

			// Every match in canonical order, then the page. A cursor is the id of the first entity of the next page, so a page
			// stays correct when entities before it are created or destroyed between calls.
			std::vector<ConstEntity> matches;
			std::as_const(*scene).ForEachCanonical([&](ConstEntity entity)
			{
				if (!params.Where.Name.empty() && !Utils::MatchesNamePattern(entity.GetName(), params.Where.Name))
					return;
				if (!params.Where.Tag.empty() && !entity.HasTag(params.Where.Tag))
					return;
				if (required != nullptr && !required->GetHostOps()->Has(entity))
					return;
				if (under.has_value())
				{
					bool inside = false;
					for (ConstEntity ancestor = entity; ancestor.IsValid() && !inside; ancestor = ancestor.GetParent())
						inside = ancestor.GetUUID() == *under;
					if (!inside)
						return;
				}
				matches.push_back(entity);
			});

			size_t first = 0;
			if (!params.Cursor.empty())
			{
				const std::optional<UUID> cursor = UUID::FromString(params.Cursor);
				const auto position = cursor.has_value() ? std::ranges::find_if(matches, [&cursor](ConstEntity entity)
				{
					return entity.GetUUID() == *cursor;
				})
														 : matches.end();
				if (position == matches.end())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/cursor",
						std::format("'{}' is not a cursor of this query (or its entity no longer matches)", params.Cursor),
						"start again with cursor \"\""));
				}
				first = static_cast<size_t>(position - matches.begin());
			}

			SceneQueryResult result;
			result.Total = ToAutomationCounter(matches.size());
			const size_t end = std::min(matches.size(), first + params.Limit);
			for (size_t index = first; index < end; ++index)
			{
				const ConstEntity entity = matches[index];
				SceneQueryEntity item;
				item.Id = entity.GetUUID().ToString();
				item.Name = entity.GetName();
				item.Path = scene->GetEntityPath(entity);
				const std::vector<const ComponentInfo*> components = selectAll ? Utils::GetEntityComponents(entity) : selected;
				for (const ComponentInfo* info : components)
				{
					if (!info->GetHostOps()->Has(entity))
						continue;
					ENGINE_TRY_ASSIGN(Json json, ComponentAccess::GetComponentJson(entity, info->GetName()));
					item.Components.emplace(info->GetName(), VariantValue(std::move(json)));
				}
				result.Entities.push_back(std::move(item));
			}
			if (end < matches.size())
				result.NextCursor = matches[end].GetUUID().ToString();
			return result;
		}

		Result<SceneGetResult> SceneGet(EditorMethodContext& context, const SceneGetParams& params)
		{
			ENGINE_TRY_ASSIGN(const Scene* scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			ENGINE_TRY_ASSIGN(Json document, SceneSerializer::ToJson(*scene));
			SceneGetResult result;
			result.Scene = VariantValue(std::move(document));
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

	}

	void RegisterSceneMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<SceneTemplate>("SceneTemplate", "What a new scene starts with.")
			.Entry(SceneTemplate::Empty, "Empty", "No entities.");

		registry.Enum<SceneTreeFormat>("SceneTreeFormat", "How scene.tree reports the hierarchy.")
			.Entry(SceneTreeFormat::Text, "Text", "A token-efficient outline with names, short ids, components and translations.")
			.Entry(SceneTreeFormat::Json, "Json", "A flat list of entries in canonical order, with depth and parent.");

		registry.Enum<SceneDiffAgainst>("SceneDiffAgainst", "What scene.diff compares the open scene with.")
			.Entry(SceneDiffAgainst::Saved, "Saved", "The scene's file on disk.")
			.Entry(SceneDiffAgainst::Revision, "Revision", "The scene as it was at an earlier revision the undo history holds.");

		registry.Enum<SceneEntityChangeKind>("SceneEntityChangeKind", "How one entity changed.")
			.Entry(SceneEntityChangeKind::Created, "Created", "The entity did not exist before.")
			.Entry(SceneEntityChangeKind::Destroyed, "Destroyed", "The entity no longer exists.")
			.Entry(SceneEntityChangeKind::Modified, "Modified", "The entity exists in both and differs.");

		registry.Struct<SceneSummary>("SceneSummary", "The open scene.")
			.Field("path", &SceneSummary::Path, "The project-relative path of its file; empty for a scene never saved.")
			.Field("name", &SceneSummary::Name, "The scene's name.")
			.Field("revision", &SceneSummary::Revision, "The editor's revision, which grows with every change.")
			.Field("dirty", &SceneSummary::Dirty, "Whether the scene has unsaved changes.")
			.Field("entityCount", &SceneSummary::EntityCount, "The number of entities.");

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
			.Field("save", &SceneNewParams::Save, "Save the open scene first when it has unsaved changes.")
			.Field("discardChanges", &SceneNewParams::DiscardChanges, "Drop the open scene's unsaved changes.");

		registry.Struct<SceneNewResult>("SceneNewResult", "The new open scene.")
			.Field("scene", &SceneNewResult::Scene, "The new scene.")
			.Field("undoIndex", &SceneNewResult::UndoIndex, "Always 0: opening a scene starts a new history.");

		registry.Struct<SceneOpenParams>("SceneOpenParams", "The params of scene.open.")
			.Field("path", &SceneOpenParams::Path, "The scene's project-relative path.")
			.Field("save", &SceneOpenParams::Save, "Save the open scene first when it has unsaved changes.")
			.Field("discardChanges", &SceneOpenParams::DiscardChanges, "Drop the open scene's unsaved changes.")
			.Field("reload", &SceneOpenParams::Reload, "Read the open scene's own file again (a version changed on disk).")
			.Field("repair", &SceneOpenParams::Repair, "Load with structural repairs and report them; the scene is then unsaved.");

		registry.Struct<SceneOpenResult>("SceneOpenResult", "The opened scene and what loading it reported.")
			.Field("scene", &SceneOpenResult::Scene, "The open scene.")
			.Field("migrated", &SceneOpenResult::Migrated, "The file was in an older format; saving it, or project.upgrade, rewrites it.")
			.Field("diagnostics", &SceneOpenResult::Diagnostics, "The load warnings, also logged.")
			.Field("repairs", &SceneOpenResult::Repairs, "The repairs a repair load applied.");

		registry.Struct<SceneSaveParams>("SceneSaveParams", "The params of scene.save.")
			.Field("path", &SceneSaveParams::Path, "Where to save, project-relative; empty for the scene's own file.");

		registry.Struct<SceneSaveResult>("SceneSaveResult", "The saved scene.")
			.Field("scene", &SceneSaveResult::Scene, "The open scene after saving.")
			.Field("file", &SceneSaveResult::File, "The project-relative file written.");

		registry.Struct<SceneTreeParams>("SceneTreeParams", "The params of scene.tree.")
			.Field("root", &SceneTreeParams::Root, "The entity whose subtree to show; empty for the whole scene.")
			.Field("depth", &SceneTreeParams::Depth, "The most levels shown; 0 for every level.")
			.Field("format", &SceneTreeParams::Format, "Text (an outline) or Json (a flat list).")
			.Field("target", &SceneTreeParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<SceneTreeEntry>("SceneTreeEntry", "One entity of scene.tree's JSON list.")
			.Field("id", &SceneTreeEntry::Id, "The entity's id.")
			.Field("name", &SceneTreeEntry::Name, "The entity's name.")
			.Field("path", &SceneTreeEntry::Path, "The entity's path.")
			.Field("parent", &SceneTreeEntry::Parent, "The parent's id; empty for a root or for the requested root.")
			.Field("depth", &SceneTreeEntry::Depth, "The depth below the listed roots, 0 for a root.")
			.Field("active", &SceneTreeEntry::Active, "The entity's own active flag.")
			.Field("components", &SceneTreeEntry::Components, "The entity's components in registry order.")
			.Field("childCount", &SceneTreeEntry::ChildCount, "The number of children.")
			.Field("childrenOmitted", &SceneTreeEntry::ChildrenOmitted, "The depth limit left out its children.");

		registry.Struct<SceneTreeResult>("SceneTreeResult", "The scene hierarchy.")
			.Field("scene", &SceneTreeResult::Scene, "The scene.")
			.Field("text", &SceneTreeResult::Text, "The outline (format text).")
			.Field("entities", &SceneTreeResult::Entities, "The entities in canonical order (format json).");

		registry.Struct<SceneQueryWhere>("SceneQueryWhere", "What scene.query matches: every given criterion holds.")
			.Field("name", &SceneQueryWhere::Name, "The entity name; '*' matches any run of characters and '?' one character.")
			.Field("tag", &SceneQueryWhere::Tag, "A tag the entity has.")
			.Field("component", &SceneQueryWhere::Component, "A component the entity has.")
			.Field("path", &SceneQueryWhere::Path, "An entity whose subtree (itself included) the matches are in.");

		registry.Struct<SceneQueryParams>("SceneQueryParams", "The params of scene.query.")
			.Field("where", &SceneQueryParams::Where, "What to match.")
			.Field("select", &SceneQueryParams::Select, "The components whose JSON each match carries; \"all\" for every one.")
			.Field("limit", &SceneQueryParams::Limit, "The most matches in one page.", { .Min = 1.0, .Max = 1000.0 })
			.Field("cursor", &SceneQueryParams::Cursor, "The nextCursor of the previous page; empty for the first.")
			.Field("target", &SceneQueryParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<SceneQueryEntity>("SceneQueryEntity", "One entity scene.query matched.")
			.Field("id", &SceneQueryEntity::Id, "The entity's id.")
			.Field("name", &SceneQueryEntity::Name, "The entity's name.")
			.Field("path", &SceneQueryEntity::Path, "The entity's path.")
			.Field("components", &SceneQueryEntity::Components, "The selected components' JSON, by registry name.");

		registry.Struct<SceneQueryResult>("SceneQueryResult", "One page of scene.query matches, in canonical order.")
			.Field("entities", &SceneQueryResult::Entities, "The matches of this page.")
			.Field("total", &SceneQueryResult::Total, "The matches over every page.")
			.Field("nextCursor", &SceneQueryResult::NextCursor, "The cursor of the next page; empty when this was the last.");

		registry.Struct<SceneGetParams>("SceneGetParams", "The params of scene.get.")
			.Field("target", &SceneGetParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<SceneGetResult>("SceneGetResult", "A scene's canonical document.")
			.Field("scene", &SceneGetResult::Scene, "The document, exactly as a scene file holds it.");

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
	}

	void RegisterSceneMethods(MethodRegistry& methods)
	{
		Json newExample = Json::object();
		newExample["path"] = "Assets/Scenes/Main.scene";
		methods.Add(
			{
				.Name = "scene.new",
				.Description = "Creates a scene file at path and makes it the open scene. With unsaved changes in the open scene it needs save "
							   "or discardChanges.",
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
				.Description = "Opens the scene at path (one scene is open at a time). With unsaved changes in the open scene it needs save or "
							   "discardChanges; reload reads the open scene again; repair loads a damaged file with reported repairs.",
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
				.Examples = { { .Description = "Save the open scene.", .Params = Json::object() } },
			},
			&Automation::SceneSave);

		Json treeExample = Json::object();
		treeExample["depth"] = 2;
		treeExample["format"] = "Text";
		methods.Add(
			{
				.Name = "scene.tree",
				.Description = "Shows the scene hierarchy: a compact text outline (names, short ids, components, translations) or a flat JSON "
							   "list, optionally below one entity and to a depth.",
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Outline the top two levels.", .Params = treeExample } },
			},
			&Automation::SceneTree);

		Json queryExample = Json::object();
		queryExample["where"] = Json::object();
		queryExample["where"]["name"] = "Cell*";
		queryExample["where"]["component"] = "MeshRenderer";
		queryExample["select"] = Json::array({ "Transform" });
		queryExample["limit"] = 50;
		methods.Add(
			{
				.Name = "scene.query",
				.Description = "Finds entities by name pattern, tag, component and subtree, in canonical order and paginated, optionally with "
							   "the JSON of selected components.",
				.RequiredParams = { "where" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Find the cells with their transforms.", .Params = queryExample } },
			},
			&Automation::SceneQuery);

		methods.Add(
			{
				.Name = "scene.get",
				.Description = "Returns the scene's whole canonical document, as its file holds it (written to a file when large).",
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the open scene.", .Params = Json::object() } },
			},
			&Automation::SceneGet);

		Json diffExample = Json::object();
		diffExample["against"] = "Saved";
		methods.Add(
			{
				.Name = "scene.diff",
				.Description = "Reports what changed per entity, as RFC 6902 patches, against the saved file or an earlier revision.",
				.RequiredParams = { "against" },
				.ExposeAsTool = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Review the unsaved changes.", .Params = diffExample } },
			},
			&Automation::SceneDiff);
	}

}
