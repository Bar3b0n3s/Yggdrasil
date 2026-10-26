#include "EditorPCH.h"
#include "EditorCore/Automation/EntityMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <map>
#include <optional>
#include <set>

namespace Engine {

	namespace Utils {

		// An undo label for a list method: "<verb> Entity '<name>'" for one entity, "<verb> <n> Entities" for more.
		static std::string MakeListLabel(std::string_view verb, const std::vector<Entity>& entities)
		{
			return entities.size() == 1 ? std::format("{} Entity '{}'", verb, entities.front().GetName())
										: std::format("{} {} Entities", verb, entities.size());
		}

		static Status ValidateEntityName(const std::string& name, std::string_view pointer)
		{
			if (name.empty())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, "an entity name must not be empty"));
			return {};
		}

		static Status ValidateTags(const std::vector<std::string>& tags, std::string_view pointer)
		{
			for (size_t index = 0; index < tags.size(); ++index)
			{
				if (tags[index].empty())
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, index), "a tag must not be empty"));
			}
			return {};
		}

		// Removes the listed components, each after the components that require it (so ["RigidBody", "SphereCollider"]
		// works in any order). A removal that can never succeed is attempted anyway, which reports why.
		static Status RemoveComponents(Entity entity, const std::vector<std::string>& names)
		{
			const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
			std::vector<std::pair<const ComponentInfo*, std::string>> pending;
			for (size_t index = 0; index < names.size(); ++index)
			{
				const std::string pointer = std::format("/removeComponents/{}", index);
				ENGINE_TRY_ASSIGN(const ComponentInfo* info, FindWritableComponent(registry, names[index], pointer));
				if (!std::ranges::any_of(pending, [info](const auto& item)
				{
					return item.first == info;
				}))
					pending.emplace_back(info, pointer);
			}
			while (!pending.empty())
			{
				// Ready when no other component still to be removed requires it.
				auto ready = std::ranges::find_if(pending, [&pending, entity](const auto& candidate)
				{
					return std::ranges::none_of(pending, [&candidate, entity](const auto& other)
					{
						return other.first != candidate.first && other.first->GetHostOps()->Has(entity)
							&& std::ranges::find(other.first->GetRequires(), candidate.first) != other.first->GetRequires().end();
					});
				});
				if (ready == pending.end())
					ready = pending.begin();
				const Status removed = ComponentAccess::RemoveComponent(entity, ready->first->GetName());
				if (!removed)
					return std::unexpected(LocateAtParam(removed.error(), ready->second));
				pending.erase(ready);
			}
			return {};
		}

		// The roots of an entity list param: each listed entity that is not inside another listed entity's subtree, once, in
		// the order of the list, with the index of the reference that names it there.
		struct EntityRoots
		{
			std::vector<Entity> Entities{};
			std::vector<size_t> Indices{}; // parallel to Entities: errors about Entities[i] are located at "/entities/<Indices[i]>"
		};

		// Resolves an entity list param, dropping repeats and entities inside another listed entity's subtree (a subtree is
		// handled with its root). Errors: InvalidArgument for an empty list; ResolveEntity's errors at "/entities/<i>".
		static Result<EntityRoots> ResolveEntityRoots(const EditorMethodContext& context, Scene& scene, const std::vector<std::string>& references)
		{
			if (references.empty())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/entities", "the entity list must not be empty"));
			std::vector<Entity> entities;
			for (size_t index = 0; index < references.size(); ++index)
			{
				ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(scene, references[index], std::format("/entities/{}", index)));
				entities.push_back(entity);
			}

			std::set<UUID> listed;
			for (const Entity entity : entities)
				listed.insert(entity.GetUUID());
			EntityRoots roots;
			std::set<UUID> taken;
			for (size_t index = 0; index < entities.size(); ++index)
			{
				const Entity entity = entities[index];
				bool insideAnother = false;
				for (Entity ancestor = entity.GetParent(); ancestor.IsValid() && !insideAnother; ancestor = ancestor.GetParent())
					insideAnother = listed.contains(ancestor.GetUUID());
				if (!insideAnother && taken.insert(entity.GetUUID()).second)
				{
					roots.Entities.push_back(entity);
					roots.Indices.push_back(index);
				}
			}
			return roots;
		}

		// `root` and its subtree in canonical order (parents before children).
		static std::vector<Entity> CollectSubtree(Entity root)
		{
			std::vector<Entity> subtree;
			std::vector<Entity> stack = { root };
			while (!stack.empty())
			{
				const Entity entity = stack.back();
				stack.pop_back();
				subtree.push_back(entity);
				const std::span<const UUID> children = entity.GetChildren();
				for (auto child = children.rbegin(); child != children.rend(); ++child)
				{
					const Entity childEntity = root.GetScene()->FindEntityByID(*child);
					if (childEntity.IsValid())
						stack.push_back(childEntity);
				}
			}
			return subtree;
		}

		// Rewrites only typed EntityRefs. Variant schemas distinguish references from strings with identical UUID text;
		// unresolved values remain verbatim, as they do when a scene is loaded.
		static void RemapEntityRefs(Json& value, const TypeInfo& type, const std::map<UUID, UUID>& remap,
			const ResolveContext& resolve, const FieldInfo* field = nullptr)
		{
			switch (type.GetKind())
			{
				case FieldType::EntityRef:
				{
					const std::optional<UUID> id = value.is_string() ? UUID::FromString(JsonReader(value).ReadString().value_or(std::string())) : std::nullopt;
					const auto mapped = id.has_value() ? remap.find(*id) : remap.end();
					if (mapped != remap.end())
						value = mapped->second.ToString();
					return;
				}
				case FieldType::Array:
					if (value.is_array() && type.GetElement() != nullptr)
					{
						for (Json& element : value)
							RemapEntityRefs(element, *type.GetElement(), remap, resolve, type.GetElementSchema() != nullptr ? type.GetElementSchema() : field);
					}
					return;
				case FieldType::Map:
					if (value.is_object() && type.GetElement() != nullptr)
					{
						for (auto member = value.begin(); member != value.end(); ++member)
						{
							ResolveContext child = resolve;
							child.Key = member.key();
							RemapEntityRefs(member.value(), *type.GetElement(), remap, child, field);
						}
					}
					return;
				case FieldType::Struct:
					if (value.is_object() && type.GetStruct() != nullptr)
					{
						const JsonReader owner(value);
						ResolveContext child = resolve;
						child.Owner = nullptr;
						child.OwnerType = type.GetStruct();
						child.OwnerJson = &owner;
						child.Key = {};
						for (auto member = value.begin(); member != value.end(); ++member)
						{
							const FieldInfo* memberField = type.GetStruct()->FindField(member.key());
							if (memberField != nullptr)
								RemapEntityRefs(member.value(), memberField->GetType(), remap, child, memberField);
						}
					}
					return;
				case FieldType::Variant:
					if (field != nullptr && field->GetResolver() != nullptr)
					{
						const auto schema = field->ResolveVariant(resolve);
						if (schema && *schema != field)
						{
							ResolveContext child = resolve;
							child.Key = {};
							RemapEntityRefs(value, (*schema)->GetType(), remap, child, *schema);
						}
					}
					return;
				case FieldType::Bool:
				case FieldType::Int32:
				case FieldType::UInt32:
				case FieldType::Float:
				case FieldType::Vec2:
				case FieldType::Vec3:
				case FieldType::Vec4:
				case FieldType::Quat:
				case FieldType::Color3:
				case FieldType::Color4:
				case FieldType::Bool3:
				case FieldType::String:
				case FieldType::AssetRef:
				case FieldType::Enum:
					return;
			}
		}

		// Duplicate checks: prefab instances are copied by prefab.instantiate (their member ids derive from the instance root,
		// §5.5), and a unique-per-scene component cannot exist twice. Errors: InvalidState naming the entity and component.
		static Status CheckDuplicable(const std::vector<Entity>& subtree)
		{
			for (const Entity entity : subtree)
			{
				for (const ComponentInfo* info : GetEntityComponents(entity))
				{
					if (info->HasFlag(ComponentFlags::Hidden))
					{
						return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' is part of a prefab instance ('{}')", entity.GetScene()->GetEntityPath(entity), info->GetName()))
								.WithHint("instantiate the prefab again instead of duplicating the instance"));
					}
					if (info->HasFlag(ComponentFlags::UniquePerScene))
					{
						return MakeError(ErrorCode::InvalidState, "'{}' has '{}', which only one entity of a scene may have",
							entity.GetScene()->GetEntityPath(entity), info->GetName());
					}
				}
			}
			return {};
		}

		// Copies `root` and its subtree through the serializer with fresh ids, internal EntityRefs remapped to the copies, and
		// inserts the copy right after `root` among its siblings. Returns the copy's root.
		static Result<Entity> DuplicateSubtree(Scene& scene, Entity root, const IFieldSchemaSource* schemas)
		{
			const std::vector<Entity> subtree = CollectSubtree(root);
			ENGINE_TRY(CheckDuplicable(subtree));

			std::map<UUID, UUID> remap;
			for (const Entity entity : subtree)
			{
				UUID copy = scene.GetUUIDGenerator().Next();
				while (scene.FindEntityByID(copy).IsValid() || std::ranges::any_of(remap, [copy](const auto& pair)
				{
					return pair.second == copy;
				}))
					copy = scene.GetUUIDGenerator().Next();
				remap.emplace(entity.GetUUID(), copy);
			}

			const TypeRegistry& registry = scene.GetTypeRegistry();
			const ResolveContext resolve{ .Registry = &registry, .Key = {}, .Schemas = schemas };
			std::vector<Json> documents;
			for (const Entity entity : subtree)
			{
				ENGINE_TRY_ASSIGN(Json document, SceneSerializer::EntityToJson(entity));
				document["ID"] = remap.find(entity.GetUUID())->second.ToString();
				const auto parent = document.find("Parent");
				if (entity != root && parent != document.end() && parent->is_string())
				{
					const std::optional<UUID> parentId = UUID::FromString(JsonReader(*parent).ReadString().value_or(std::string()));
					const auto mapped = parentId.has_value() ? remap.find(*parentId) : remap.end();
					if (mapped != remap.end())
						*parent = mapped->second.ToString();
				}
				const auto components = document.find("Components");
				if (components != document.end() && components->is_object())
				{
					for (auto component = components->begin(); component != components->end(); ++component)
					{
						if (const ComponentInfo* info = registry.FindComponent(component.key()))
							RemapEntityRefs(component.value(), info->GetType(), remap, resolve);
					}
				}
				documents.push_back(std::move(document));
			}

			LoadOptions options;
			options.Schemas = schemas;
			LoadReport report;
			Entity copyRoot;
			for (size_t index = 0; index < documents.size(); ++index)
			{
				const std::optional<uint32_t> siblingIndex = index == 0 ? std::optional<uint32_t>(root.GetSiblingIndex() + 1) : std::nullopt;
				ENGINE_TRY_ASSIGN(const Entity created, SceneSerializer::EntityFromJson(scene, JsonReader(documents[index]), siblingIndex, options, report));
				if (index == 0)
					copyRoot = created;
			}
			return copyRoot;
		}

	}

	namespace Automation {

		Result<EntityCreateResult> EntityCreate(EditorMethodContext& context, const EntityCreateParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY(Utils::ValidateEntityName(params.Name, "/name"));
			ENGINE_TRY(Utils::ValidateTags(params.Tags, "/tags"));
			Entity parent;
			if (!params.Parent.empty())
			{
				ENGINE_TRY_ASSIGN(parent, context.ResolveEntity(*scene, params.Parent, "/parent"));
			}

			ENGINE_TRY(Utils::CheckPlayEntityCapacity(context, *scene, 1));
			SceneEdit edit(context.GetEditor(), std::format("Create Entity '{}'", params.Name));
			const Entity entity = parent.IsValid() ? scene->CreateEntity(params.Name, parent) : scene->CreateEntity(params.Name);
			if (context.HasParam("index"))
				ENGINE_TRY(scene->SetParent(entity, parent, params.Index, false));
			for (const std::string& tag : params.Tags)
				entity.AddTag(tag);
			if (!params.Active)
				entity.SetActive(false);
			ENGINE_TRY(Utils::WriteComponents(context.GetEditor(), entity, params.Components));
			const UUID id = entity.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityCreateResult result;
			result.Entity = context.MakeEntitySummary(scene->FindEntityByID(id));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityUpdateResult> EntityUpdate(EditorMethodContext& context, const EntityUpdateParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			const bool renames = context.HasParam("name");
			const bool retags = context.HasParam("tags");
			if (renames)
				ENGINE_TRY(Utils::ValidateEntityName(params.Name, "/name"));
			if (retags)
				ENGINE_TRY(Utils::ValidateTags(params.Tags, "/tags"));

			SceneEdit edit(context.GetEditor(), std::format("Update Entity '{}'", entity.GetName()));
			if (renames)
				entity.SetName(params.Name);
			if (context.HasParam("active"))
				entity.SetActive(params.Active);
			if (retags)
			{
				const std::span<const std::string> current = entity.GetTags();
				const std::vector<std::string> previous(current.begin(), current.end());
				for (const std::string& tag : previous)
					entity.RemoveTag(tag);
				for (const std::string& tag : params.Tags)
					entity.AddTag(tag);
			}
			ENGINE_TRY(Utils::RemoveComponents(entity, params.RemoveComponents));
			ENGINE_TRY(Utils::WriteComponents(context.GetEditor(), entity, params.Components));
			const UUID id = entity.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityUpdateResult result;
			ENGINE_TRY_ASSIGN(result.Entity, Automation::MakeEntityDetails(context, scene->FindEntityByID(id), std::nullopt, false));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityDestroyResult> EntityDestroy(EditorMethodContext& context, const EntityDestroyParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const Utils::EntityRoots roots, Utils::ResolveEntityRoots(context, *scene, params.Entities));

			std::set<UUID> destroyed;
			for (const Entity root : roots.Entities)
			{
				for (const Entity entity : Utils::CollectSubtree(root))
					destroyed.insert(entity.GetUUID());
			}
			// A prefab member that is not its instance's root cannot be deleted from the edit scene (the instance's next update
			// would rebuild it); the play scene's changes are transient and no update rebuilds them, as for scripts' Scene.Destroy.
			if (params.Target == SceneTarget::Edit)
			{
				for (size_t index = 0; index < roots.Entities.size(); ++index)
				{
					const std::optional<Utils::KeptPrefabMember> kept = Utils::FindKeptPrefabMember(roots.Entities[index], destroyed);
					if (!kept)
						continue;
					const std::string member = scene->GetEntityPath(kept->Member);
					const std::string instance = scene->GetEntityPath(kept->InstanceRoot);
					// The ways out as calls to copy, their params built as JSON so that any entity name is escaped.
					const Json deactivate{ { "entity", member }, { "active", false } };
					const Json unpack{ { "instance", instance } };
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidState, std::format("/entities/{}", roots.Indices[index]),
						std::format("'{}' is a member of the prefab instance '{}' and cannot be deleted: the instance's next update would rebuild it",
							member, instance),
						std::format("deactivate it instead (entity.update {}), or unpack the instance first (prefab.unpack {})",
							deactivate.dump(-1, ' ', false, Json::error_handler_t::replace), unpack.dump(-1, ' ', false, Json::error_handler_t::replace))));
				}
			}

			EntityDestroyResult result;
			for (const UUID id : destroyed)
				result.Destroyed.push_back(id.ToString());
			std::sort(result.Destroyed.begin(), result.Destroyed.end());

			SceneEdit edit(context.GetEditor(), Utils::MakeListLabel("Destroy", roots.Entities));
			for (const Entity root : roots.Entities)
				scene->DestroyEntity(root);
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityDuplicateResult> EntityDuplicate(EditorMethodContext& context, const EntityDuplicateParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const Utils::EntityRoots roots, Utils::ResolveEntityRoots(context, *scene, params.Entities));
			ENGINE_TRY(Utils::CheckPlayEntityCapacity(context, *scene, Utils::CountSubtreeEntities(roots.Entities)));
			ENGINE_TRY_ASSIGN(const auto schemas, context.GetEditor().GetScriptSchemaSnapshot());

			SceneEdit edit(context.GetEditor(), Utils::MakeListLabel("Duplicate", roots.Entities));
			std::vector<UUID> copies;
			for (const Entity root : roots.Entities)
			{
				ENGINE_TRY_ASSIGN(const Entity copy, Utils::DuplicateSubtree(*scene, root, schemas.get()));
				copies.push_back(copy.GetUUID());
			}
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityDuplicateResult result;
			for (const UUID copy : copies)
				result.Entities.push_back(context.MakeEntitySummary(scene->FindEntityByID(copy)));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityReparentResult> EntityReparent(EditorMethodContext& context, const EntityReparentParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			Entity parent;
			if (!params.Parent.empty())
			{
				ENGINE_TRY_ASSIGN(parent, context.ResolveEntity(*scene, params.Parent, "/parent"));
			}
			const std::optional<uint32_t> index = context.HasParam("index") ? std::optional<uint32_t>(params.Index) : std::nullopt;

			SceneEdit edit(context.GetEditor(), std::format("Reparent Entity '{}'", entity.GetName()));
			const Status moved = scene->SetParent(entity, parent, index, params.KeepWorld);
			if (!moved)
				return std::unexpected(Utils::LocateAtParam(moved.error(), "/parent"));
			const UUID id = entity.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityReparentResult result;
			result.Entity = context.MakeEntitySummary(scene->FindEntityByID(id));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

	}

	void RegisterEditorEntityMethodTypes(TypeRegistry& registry)
	{
		constexpr std::string_view ComponentsDescription =
			"Component values by registry name, each an object of field values (missing fields keep their values); an Entity value takes 16 hex "
			"digits, a unique id prefix of at least 6 or a path such as \"/Game/ScoreText\", resolved in the target scene; see component.schema. "
			"Native components (Native.<Name>) are written with native.setComponents.";
		constexpr std::string_view TargetDescription = "The scene to change; omitted: the edit scene. \"play\" changes the play scene transiently.";

		registry.Struct<EntityCreateParams>("EntityCreateParams", "The params of entity.create.")
			.Field("name", &EntityCreateParams::Name, "The new entity's name.")
			.Field("parent", &EntityCreateParams::Parent, "The parent entity; empty for a root.")
			.Field("index", &EntityCreateParams::Index, "The position among its siblings; omitted: last.")
			.Field("active", &EntityCreateParams::Active, "Whether the entity starts active.")
			.Field("tags", &EntityCreateParams::Tags, "The entity's tags.")
			.VariantField("components", &EntityCreateParams::Components, ComponentsDescription, &ResolveComponentValue)
			.Field("target", &EntityCreateParams::Target, TargetDescription);

		registry.Struct<EntityCreateResult>("EntityCreateResult", "The created entity.")
			.Field("entity", &EntityCreateResult::Entity, "The new entity.")
			.Field("undoIndex", &EntityCreateResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");

		registry.Struct<EntityUpdateParams>("EntityUpdateParams", "The params of entity.update: only the given members change.")
			.Field("entity", &EntityUpdateParams::Entity, "The entity: 16 hex digits, a unique id prefix of at least 6, or a path.")
			.Field("name", &EntityUpdateParams::Name, "The new name.")
			.Field("active", &EntityUpdateParams::Active, "The new active flag.")
			.Field("tags", &EntityUpdateParams::Tags, "The new tag list (replaces the old one).")
			.VariantField("components", &EntityUpdateParams::Components,
				"Component values by registry name, merged into the existing components; missing components are added. An Entity value takes 16 hex "
				"digits, a unique id prefix of at least 6 or a path such as \"/Game/ScoreText\", resolved in the target scene. Native components "
				"(Native.<Name>) are written with native.setComponents.",
				&ResolveComponentValue)
			.Field("removeComponents", &EntityUpdateParams::RemoveComponents,
				"Components to remove (before the components are written); native.setComponents removes a native one (null).")
			.Field("target", &EntityUpdateParams::Target, TargetDescription);

		registry.Struct<EntityUpdateResult>("EntityUpdateResult", "The updated entity.")
			.Field("entity", &EntityUpdateResult::Entity, "The entity after the update, with every component.")
			.Field("undoIndex", &EntityUpdateResult::UndoIndex, "The command's undo index; 0 in a dry run, a batch, or when nothing changed.");

		registry.Struct<EntityDestroyParams>("EntityDestroyParams", "The params of entity.destroy.")
			.Field("entities", &EntityDestroyParams::Entities, "The entities to destroy, each with its subtree.")
			.Field("target", &EntityDestroyParams::Target, TargetDescription);

		registry.Struct<EntityDestroyResult>("EntityDestroyResult", "The destroyed entities.")
			.Field("destroyed", &EntityDestroyResult::Destroyed, "The ids of every destroyed entity, subtrees included, sorted.")
			.Field("undoIndex", &EntityDestroyResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");

		registry.Struct<EntityDuplicateParams>("EntityDuplicateParams", "The params of entity.duplicate.")
			.Field("entities", &EntityDuplicateParams::Entities, "The entities to copy, each with its subtree.")
			.Field("target", &EntityDuplicateParams::Target, TargetDescription);

		registry.Struct<EntityDuplicateResult>("EntityDuplicateResult", "The copies.")
			.Field("entities", &EntityDuplicateResult::Entities, "The copies' roots, in the order of the entities param.")
			.Field("undoIndex", &EntityDuplicateResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");

		registry.Struct<EntityReparentParams>("EntityReparentParams", "The params of entity.reparent.")
			.Field("entity", &EntityReparentParams::Entity, "The entity to move.")
			.Field("parent", &EntityReparentParams::Parent, "The new parent; empty for the root list.")
			.Field("index", &EntityReparentParams::Index, "The position among its new siblings; omitted: last.")
			.Field("keepWorld", &EntityReparentParams::KeepWorld, "Keep the world transform (else the local transform is kept).")
			.Field("target", &EntityReparentParams::Target, TargetDescription);

		registry.Struct<EntityReparentResult>("EntityReparentResult", "The moved entity.")
			.Field("entity", &EntityReparentResult::Entity, "The entity with its new path.")
			.Field("undoIndex", &EntityReparentResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");
	}

	void RegisterEditorEntityMethods(MethodRegistry& methods)
	{
		Json createExample = Json::object();
		createExample["name"] = "Camera";
		createExample["components"] = Json::object();
		createExample["components"]["Transform"] = Json::object();
		createExample["components"]["Transform"]["Translation"] = Json::array({ 4.5, 9.5, 20 });
		createExample["components"]["Camera"] = Json::object();
		createExample["components"]["Camera"]["Projection"] = "Orthographic";
		createExample["components"]["Camera"]["OrthographicSize"] = 11;
		createExample["components"]["Camera"]["Primary"] = true;
		methods.Add(
			{
				.Name = "entity.create",
				.Description = "Creates an entity with a name, parent, position among its siblings, active flag, tags and component values, as "
							   "one undoable command.",
				.RequiredParams = { "name" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Create an orthographic primary camera.", .Params = createExample } },
			},
			&Automation::EntityCreate);

		Json updateExample = Json::object();
		updateExample["entity"] = "/Ball";
		updateExample["components"] = Json::object();
		updateExample["components"]["RigidBody"] = Json::object();
		updateExample["components"]["RigidBody"]["Mass"] = 2;
		methods.Add(
			{
				.Name = "entity.update",
				.Description = "Changes only what is given: name, active flag, tags, component fields (merged; missing components are added) "
							   "and removed components, as one undoable command.",
				.RequiredParams = { "entity" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Give the ball a rigid body of 2 kg.", .Params = updateExample } },
			},
			&Automation::EntityUpdate);

		Json destroyExample = Json::object();
		destroyExample["entities"] = Json::array({ "/Cell" });
		methods.Add(
			{
				.Name = "entity.destroy",
				.Description = "Destroys entities with their subtrees, as one undoable command. In the edit scene a member of a prefab instance "
							   "that is not its root is refused: deactivate it, or unpack the instance first.",
				.RequiredParams = { "entities" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Destroy the cell template.", .Params = destroyExample } },
			},
			&Automation::EntityDestroy);

		Json duplicateExample = Json::object();
		duplicateExample["entities"] = Json::array({ "/Track/Piece[0]" });
		methods.Add(
			{
				.Name = "entity.duplicate",
				.Description = "Copies entities with their subtrees, with fresh ids and internal references remapped, each right after its "
							   "original, as one undoable command.",
				.RequiredParams = { "entities" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Copy the first track piece.", .Params = duplicateExample } },
			},
			&Automation::EntityDuplicate);

		Json reparentExample = Json::object();
		reparentExample["entity"] = "/Board";
		reparentExample["parent"] = "/Game";
		reparentExample["index"] = 0;
		methods.Add(
			{
				.Name = "entity.reparent",
				.Description = "Moves an entity under another parent (or to the root list with parent \"\") at a sibling position, keeping its "
							   "world transform unless keepWorld is false, as one undoable command.",
				.RequiredParams = { "entity", "parent" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Move the board under the game, first.", .Params = reparentExample } },
			},
			&Automation::EntityReparent);
	}

}
