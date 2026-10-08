#include "EnginePCH.h"
#include "Engine/Automation/Methods/SceneMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/ComponentInfo.h"
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

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <utility>

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

		// Text shown for a component in the scene.tree outline is cut to this many characters.
		constexpr size_t MaxOutlineTextLength = 24;

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

		// The outline's file name: the last component of the summary's path, or "<name> (unsaved)" for a scene never saved.
		static std::string FormatOutlineFile(const SceneSummary& summary)
		{
			if (summary.Path.empty())
				return std::format("{} (unsaved)", summary.Name);
			const size_t slash = summary.Path.rfind('/');
			return slash == std::string::npos ? summary.Path : summary.Path.substr(slash + 1);
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

	}

	namespace Automation {

		Result<SceneTreeResult> SceneTree(AutomationMethodContext& context, const SceneTreeParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));

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
			result.Scene = context.MakeSceneSummary(*scene);
			TreeWalk walk;
			walk.Target = scene;
			walk.DepthLimit = params.Depth;
			if (params.Format == SceneTreeFormat::JsonList)
			{
				walk.Entries = &result.Entities;
			}
			else
			{
				walk.Text = &result.Text;
				result.Text = std::format("{}  rev {}  {}{}\n", Utils::FormatOutlineFile(result.Scene), result.Scene.Revision, result.Scene.Dirty ? "dirty  " : "",
					Utils::FormatCount(scene->GetEntityCount(), "entity", "entities"));
			}
			for (size_t index = 0; index < roots.size(); ++index)
			{
				if (roots[index].IsValid())
					Utils::WalkTree(walk, roots[index], index + 1 == roots.size(), !params.Root.empty());
			}
			return result;
		}

		Result<SceneQueryResult> SceneQuery(AutomationMethodContext& context, const SceneQueryParams& params)
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

		Result<SceneGetResult> SceneGet(AutomationMethodContext& context, const SceneGetParams& params)
		{
			ENGINE_TRY_ASSIGN(const Scene* scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			ENGINE_TRY_ASSIGN(Json document, SceneSerializer::ToJson(*scene));
			SceneGetResult result;
			result.Scene = VariantValue(std::move(document));
			return result;
		}

	}

	void RegisterSceneMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<SceneTreeFormat>("SceneTreeFormat", "How scene.tree reports the hierarchy.")
			.Entry(SceneTreeFormat::Text, "Text", "A token-efficient outline with names, short ids, components and translations.")
			.Entry(SceneTreeFormat::JsonList, "Json", "A flat list of entries in canonical order, with depth and parent.");

		registry.Struct<SceneSummary>("SceneSummary", "The open scene.")
			.Field("path", &SceneSummary::Path, "The project-relative path of its file; empty for a scene never saved.")
			.Field("name", &SceneSummary::Name, "The scene's name.")
			.Field("revision", &SceneSummary::Revision, "The host's revision (the play scene's in the Runtime), which grows with every change.")
			.Field("dirty", &SceneSummary::Dirty, "Whether the scene has unsaved changes.")
			.Field("entityCount", &SceneSummary::EntityCount, "The number of entities.");

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
	}

	void RegisterSceneMethods(MethodRegistry& methods)
	{
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
	}

}
