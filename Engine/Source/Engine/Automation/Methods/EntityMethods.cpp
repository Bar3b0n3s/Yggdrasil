#include "EnginePCH.h"
#include "Engine/Automation/Methods/EntityMethods.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Aabb.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/EntityBounds.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <format>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		// Which components entity.get reports: every one (nullopt) or the named ones. Errors: InvalidArgument at "/components"
		// for a value that is neither "all" nor an array of names; NotFound or InvalidArgument for a name.
		static Result<std::optional<std::vector<const ComponentInfo*>>> ReadComponentSelection(const TypeRegistry& registry, const VariantValue& value,
			bool given)
		{
			const Json& json = value.Get();
			if (!given || json.is_null())
				return std::optional<std::vector<const ComponentInfo*>>();
			if (json.is_string() && JsonReader(json).ReadString().value_or(std::string()) == "all")
				return std::optional<std::vector<const ComponentInfo*>>();
			if (!json.is_array())
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/components", "components must be \"all\" or an array of component names",
					"pass [\"Transform\", \"Camera\"] or \"all\""));
			}

			std::vector<const ComponentInfo*> selected;
			for (size_t index = 0; index < json.size(); ++index)
			{
				const std::string pointer = std::format("/components/{}", index);
				const Result<std::string> name = JsonReader(json[index]).ReadString();
				if (!name)
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, "a component name must be a string"));
				const ComponentInfo* info = registry.FindComponent(*name);
				if (info == nullptr || !info->HasFlag(ComponentFlags::Serializable))
				{
					std::vector<std::string> suggestions = registry.SuggestComponentNames(*name);
					return std::unexpected(MakeParamError(ErrorCode::NotFound, pointer, std::format("no component '{}'", *name), MakeDidYouMeanHint(suggestions)));
				}
				if (info->HasFlag(ComponentFlags::EntityLevel))
				{
					return std::unexpected(
						MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is an entity member, reported as name, tags, parent and active", *name)));
				}
				selected.push_back(info);
			}
			return std::optional<std::vector<const ComponentInfo*>>(std::move(selected));
		}

	}

	namespace Automation {

		Result<EntityDetails> MakeEntityDetails(const AutomationMethodContext& context, ConstEntity entity,
			const std::optional<std::vector<const ComponentInfo*>>& selection, bool children)
		{
			const Scene& scene = *entity.GetScene();
			EntityDetails details;
			details.Id = entity.GetUUID().ToString();
			details.Name = entity.GetName();
			details.Path = scene.GetEntityPath(entity);
			details.Parent = entity.GetParent().IsValid() ? entity.GetParent().GetUUID().ToString() : std::string();
			details.Active = entity.IsActiveSelf();
			details.ActiveInHierarchy = entity.IsActive();
			const std::span<const std::string> tags = entity.GetTags();
			details.Tags.assign(tags.begin(), tags.end());

			const std::vector<const ComponentInfo*> components = selection.has_value() ? *selection : Utils::GetEntityComponents(entity);
			for (const ComponentInfo* info : components)
			{
				if (!info->GetHostOps()->Has(entity))
					continue;
				ENGINE_TRY_ASSIGN(Json json, ComponentAccess::GetComponentJson(entity, info->GetName()));
				details.Components.emplace(info->GetName(), VariantValue(std::move(json)));
			}
			if (children)
			{
				for (const UUID child : entity.GetChildren())
				{
					const ConstEntity childEntity = scene.FindEntityByID(child);
					if (childEntity.IsValid())
						details.Children.push_back(context.MakeEntitySummary(childEntity));
				}
			}
			return details;
		}

		Result<EntityGetResult> EntityGet(AutomationMethodContext& context, const EntityGetParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			ENGINE_TRY_ASSIGN(const std::optional<std::vector<const ComponentInfo*>> selection,
				Utils::ReadComponentSelection(scene->GetTypeRegistry(), params.Components, context.HasParam("components")));
			EntityGetResult result;
			ENGINE_TRY_ASSIGN(result.Entity, MakeEntityDetails(context, entity, selection, params.Children));
			return result;
		}

		Result<EntityBoundsResult> EntityBounds(AutomationMethodContext& context, const EntityBoundsParams& params)
		{
			if (params.Entities.empty())
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/entities", "give at least one entity"));
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			std::vector<Entity> entities;
			entities.reserve(params.Entities.size());
			for (size_t index = 0; index < params.Entities.size(); ++index)
			{
				ENGINE_TRY_ASSIGN(Entity entity, context.ResolveEntity(*scene, params.Entities[index], std::format("/entities/{}", index)));
				entities.push_back(entity);
			}

			AssetManager* manager = context.GetAssets();
			if (manager == nullptr)
				return MakeError(ErrorCode::Unsupported, "entity.bounds needs the host's asset manager, which this host does not have");
			// Layout feedback reads the settled state: imports and hot-reload swaps requested so far are published first (§8.13).
			AssetManager& assets = *manager;
			assets.WaitIdle();
			const auto toArray = [](const glm::vec3& value)
			{
				return std::vector<float>{ value.x, value.y, value.z };
			};
			EntityBoundsResult result;
			result.Bounds.reserve(entities.size());
			for (const Entity entity : entities)
			{
				EntityWorldBounds bounds;
				bounds.Entity = context.MakeEntitySummary(entity);
				if (const std::optional<Aabb> box = ComputeEntityWorldBounds(entity, assets, { .IncludeDescendants = params.IncludeDescendants }); box.has_value())
				{
					bounds.HasBounds = true;
					bounds.Min = toArray(box->Min);
					bounds.Max = toArray(box->Max);
					bounds.Center = toArray(box->GetCenter());
					bounds.Size = toArray(box->GetSize());
				}
				result.Bounds.push_back(std::move(bounds));
			}
			return result;
		}

	}

	void RegisterEntityMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<EntityDetails>("EntityDetails", "One entity with its members and component values.")
			.Field("id", &EntityDetails::Id, "The entity's id.")
			.Field("name", &EntityDetails::Name, "The entity's name.")
			.Field("path", &EntityDetails::Path, "The entity's path.")
			.Field("parent", &EntityDetails::Parent, "The parent's id; empty for a root.")
			.Field("active", &EntityDetails::Active, "The entity's own active flag.")
			.Field("activeInHierarchy", &EntityDetails::ActiveInHierarchy, "Whether the entity and every ancestor are active.")
			.Field("tags", &EntityDetails::Tags, "The entity's tags.")
			.Field("components", &EntityDetails::Components, "The selected components' JSON, by registry name.")
			.Field("children", &EntityDetails::Children, "The children, when asked for.");

		registry.Struct<EntityGetParams>("EntityGetParams", "The params of entity.get.")
			.Field("entity", &EntityGetParams::Entity, "The entity: 16 hex digits, a unique id prefix of at least 6, or a path.")
			.Field("components", &EntityGetParams::Components, "An array of component names, or \"all\" (the default).")
			.Field("children", &EntityGetParams::Children, "Also list the children.")
			.Field("target", &EntityGetParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<EntityGetResult>("EntityGetResult", "One entity.")
			.Field("entity", &EntityGetResult::Entity, "The entity.");

		registry.Struct<EntityWorldBounds>("EntityWorldBounds", "One entity's world-space axis-aligned bounding box.")
			.Field("entity", &EntityWorldBounds::Entity, "The entity.")
			.Field("hasBounds", &EntityWorldBounds::HasBounds, "False when neither it nor (with includeDescendants) a descendant has a mesh.")
			.Field("min", &EntityWorldBounds::Min, "The minimum corner [x, y, z] in metres; empty without bounds.")
			.Field("max", &EntityWorldBounds::Max, "The maximum corner [x, y, z] in metres; empty without bounds.")
			.Field("center", &EntityWorldBounds::Center, "The centre [x, y, z] in metres; empty without bounds.")
			.Field("size", &EntityWorldBounds::Size, "The extent [x, y, z] in metres; empty without bounds.");

		registry.Struct<EntityBoundsParams>("EntityBoundsParams", "The params of entity.bounds.")
			.Field("entities", &EntityBoundsParams::Entities, "The entities: 16 hex digits, unique id prefixes or paths.")
			.Field("includeDescendants", &EntityBoundsParams::IncludeDescendants, "Also include every descendant's meshes.")
			.Field("target", &EntityBoundsParams::Target, "Which scene to read; the play scene while playing when absent.");

		registry.Struct<EntityBoundsResult>("EntityBoundsResult", "The world bounds of the entities, in the order given.")
			.Field("bounds", &EntityBoundsResult::Bounds, "One entry per entity.");
	}

	void RegisterEntityMethods(MethodRegistry& methods)
	{
		Json getExample = Json::object();
		getExample["entity"] = "/Game/Board";
		getExample["components"] = Json::array({ "Transform" });
		getExample["children"] = true;
		methods.Add(
			{
				.Name = "entity.get",
				.Description = "Returns one entity: id, name, path, parent, active state, tags, the JSON of all or selected components, and "
							   "optionally its children.",
				.RequiredParams = { "entity" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Read the board's transform and children.", .Params = getExample } },
			},
			&Automation::EntityGet);

		Json boundsExample = Json::object();
		boundsExample["entities"] = Json::array({ "/Track", "/Track/Piece" });
		methods.Add(
			{
				.Name = "entity.bounds",
				.Description = "Reports the world-space bounding box of each entity's meshes (and, by default, its descendants'), for layout "
							   "checks.",
				.RequiredParams = { "entities" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Measure the track and one piece.", .Params = boundsExample } },
			},
			&Automation::EntityBounds);
	}

}
