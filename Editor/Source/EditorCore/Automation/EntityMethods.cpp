#include "EditorPCH.h"
#include "EditorCore/Automation/EntityMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

#include <map>
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

		// The registered component `name` that automation may write. Errors at `pointer`: NotFound with suggestions;
		// InvalidArgument for an entity-level component (written through the entity's members) or a Hidden one.
		static Result<const ComponentInfo*> FindWritableComponent(const TypeRegistry& registry, std::string_view name, std::string_view pointer)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			if (info == nullptr || info->GetHostOps() == nullptr || !info->HasFlag(ComponentFlags::Serializable))
			{
				std::vector<std::string> suggestions = registry.SuggestComponentNames(name);
				return std::unexpected(MakeParamError(ErrorCode::NotFound, pointer, std::format("no component '{}'", name),
					suggestions.empty() ? std::string("component.list lists every component") : MakeDidYouMeanHint(suggestions)));
			}
			if (info->HasFlag(ComponentFlags::EntityLevel))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is an entity member, not a component", name),
					"set names, tags and active state with the name, tags and active params, and parents with entity.reparent"));
			}
			if (info->HasFlag(ComponentFlags::Hidden))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer,
					std::format("'{}' is maintained by the engine and cannot be written", name)));
			}
			return info;
		}

		// Writes one component's (partial) JSON `value`: added when `add`, else merged into the existing component (RFC 7386).
		// Stored fields go through ComponentAccess in one write; virtual fields (Transform.EulerAngles, WorldPosition) are set
		// after it through their setters. Errors are located below `pointer`.
		static Status WriteComponent(Entity entity, const ComponentInfo& info, const Json& value, bool add, const std::string& pointer)
		{
			if (!value.is_object())
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("the value of '{}' must be an object of fields", info.GetName())));

			Json stored = Json::object();
			std::vector<std::pair<const FieldInfo*, const Json*>> virtuals;
			for (auto member = value.begin(); member != value.end(); ++member)
			{
				const FieldInfo* field = info.FindField(member.key());
				if (field != nullptr && field->IsVirtual())
					virtuals.emplace_back(field, &member.value());
				else
					stored[member.key()] = member.value();
			}

			const Status written = add ? ComponentAccess::AddComponent(entity, info.GetName(), &stored)
									   : (stored.empty() ? Status() : ComponentAccess::PatchComponentJson(entity, info.GetName(), stored));
			if (!written)
				return std::unexpected(PrefixPointers(written.error(), pointer));

			for (const auto& [field, json] : virtuals)
			{
				const std::string fieldPointer = JsonReader::AppendPointer(pointer, field->GetName());
				Result<Value> fieldValue = ValueFromJson(JsonReader(*json), field->GetType());
				if (!fieldValue)
					return std::unexpected(PrefixPointers(fieldValue.error(), fieldPointer));
				const Status set = ComponentAccess::SetFieldValue(entity, info.GetName(), field->GetName(), *fieldValue);
				if (!set)
					return std::unexpected(PrefixPointers(set.error(), pointer));
			}
			return {};
		}

		// Writes every component of a create or update: existing ones are merged, missing ones added once the components they
		// require are present (so {"SphereCollider": ..., "RigidBody": ...} works in any order). A component that can never
		// be added is attempted anyway, which reports why (ComponentAccess::AddComponent).
		static Status WriteComponents(Entity entity, const std::map<std::string, VariantValue>& components)
		{
			const TypeRegistry& registry = entity.GetScene()->GetTypeRegistry();
			std::vector<std::pair<const ComponentInfo*, const VariantValue*>> additions;
			for (const auto& [name, value] : components)
			{
				const std::string pointer = JsonReader::AppendPointer("/components", name);
				ENGINE_TRY_ASSIGN(const ComponentInfo* info, FindWritableComponent(registry, name, pointer));
				if (info->GetHostOps()->Has(entity))
					ENGINE_TRY(WriteComponent(entity, *info, value.Get(), false, pointer));
				else
					additions.emplace_back(info, &value);
			}

			std::sort(additions.begin(), additions.end(), [](const auto& left, const auto& right)
			{
				return left.first->GetIndex() < right.first->GetIndex();
			});
			while (!additions.empty())
			{
				const auto isPending = [&additions](const ComponentInfo* component)
				{
					return std::ranges::any_of(additions, [component](const auto& addition)
					{
						return addition.first == component;
					});
				};
				auto ready = std::ranges::find_if(additions, [&isPending](const auto& addition)
				{
					return std::ranges::none_of(addition.first->GetRequires(), isPending);
				});
				if (ready == additions.end())
					ready = additions.begin();
				const std::string pointer = JsonReader::AppendPointer("/components", ready->first->GetName());
				ENGINE_TRY(WriteComponent(entity, *ready->first, ready->second->Get(), true, pointer));
				additions.erase(ready);
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

		// Which components entity.get reports: every one (nullopt) or the named ones. Errors: InvalidArgument at "/components"
		// for a value that is neither "all" nor an array of names; NotFound or InvalidArgument for a name (FindQueryComponent).
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

		static Result<EntityDetails> MakeEntityDetails(const EditorMethodContext& context, ConstEntity entity,
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

			const std::vector<const ComponentInfo*> components = selection.has_value() ? *selection : GetEntityComponents(entity);
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

		// Resolves an entity list param, dropping repeats and entities inside another listed entity's subtree (a subtree is
		// handled with its root). Errors: InvalidArgument for an empty list; ResolveEntity's errors at "/entities/<i>".
		static Result<std::vector<Entity>> ResolveEntityRoots(const EditorMethodContext& context, Scene& scene, const std::vector<std::string>& references)
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
			std::vector<Entity> roots;
			std::set<UUID> taken;
			for (const Entity entity : entities)
			{
				bool insideAnother = false;
				for (Entity ancestor = entity.GetParent(); ancestor.IsValid() && !insideAnother; ancestor = ancestor.GetParent())
					insideAnother = listed.contains(ancestor.GetUUID());
				if (!insideAnother && taken.insert(entity.GetUUID()).second)
					roots.push_back(entity);
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

		// Rewrites every EntityRef inside `value` (a JSON value of `type`) that names a key of `remap` to its value. Variant
		// values are left alone: their schemas (script fields) arrive with M13.
		static void RemapEntityRefs(Json& value, const TypeInfo& type, const std::map<UUID, UUID>& remap)
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
							RemapEntityRefs(element, *type.GetElement(), remap);
					}
					return;
				case FieldType::Map:
					if (value.is_object() && type.GetElement() != nullptr)
					{
						for (auto member = value.begin(); member != value.end(); ++member)
							RemapEntityRefs(member.value(), *type.GetElement(), remap);
					}
					return;
				case FieldType::Struct:
					if (value.is_object() && type.GetStruct() != nullptr)
					{
						for (auto member = value.begin(); member != value.end(); ++member)
						{
							const FieldInfo* field = type.GetStruct()->FindField(member.key());
							if (field != nullptr)
								RemapEntityRefs(member.value(), field->GetType(), remap);
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
				case FieldType::Variant:
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
		static Result<Entity> DuplicateSubtree(Scene& scene, Entity root)
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
							RemapEntityRefs(component.value(), info->GetType(), remap);
					}
				}
				documents.push_back(std::move(document));
			}

			LoadOptions options;
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

			SceneEdit edit(context.GetEditor(), std::format("Create Entity '{}'", params.Name));
			const Entity entity = parent.IsValid() ? scene->CreateEntity(params.Name, parent) : scene->CreateEntity(params.Name);
			if (context.HasParam("index"))
				ENGINE_TRY(scene->SetParent(entity, parent, params.Index, false));
			for (const std::string& tag : params.Tags)
				entity.AddTag(tag);
			if (!params.Active)
				entity.SetActive(false);
			ENGINE_TRY(Utils::WriteComponents(entity, params.Components));
			const UUID id = entity.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityCreateResult result;
			result.Entity = context.MakeEntitySummary(scene->FindEntityByID(id));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityGetResult> EntityGet(EditorMethodContext& context, const EntityGetParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			ENGINE_TRY_ASSIGN(const Entity entity, context.ResolveEntity(*scene, params.Entity, "/entity"));
			ENGINE_TRY_ASSIGN(const std::optional<std::vector<const ComponentInfo*>> selection,
				Utils::ReadComponentSelection(scene->GetTypeRegistry(), params.Components, context.HasParam("components")));
			EntityGetResult result;
			ENGINE_TRY_ASSIGN(result.Entity, Utils::MakeEntityDetails(context, entity, selection, params.Children));
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
			ENGINE_TRY(Utils::WriteComponents(entity, params.Components));
			const UUID id = entity.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			EntityUpdateResult result;
			ENGINE_TRY_ASSIGN(result.Entity, Utils::MakeEntityDetails(context, scene->FindEntityByID(id), std::nullopt, false));
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityDestroyResult> EntityDestroy(EditorMethodContext& context, const EntityDestroyParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const std::vector<Entity> roots, Utils::ResolveEntityRoots(context, *scene, params.Entities));

			EntityDestroyResult result;
			for (const Entity root : roots)
			{
				for (const Entity entity : Utils::CollectSubtree(root))
					result.Destroyed.push_back(entity.GetUUID().ToString());
			}
			std::sort(result.Destroyed.begin(), result.Destroyed.end());

			SceneEdit edit(context.GetEditor(), Utils::MakeListLabel("Destroy", roots));
			for (const Entity root : roots)
				scene->DestroyEntity(root);
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
		}

		Result<EntityDuplicateResult> EntityDuplicate(EditorMethodContext& context, const EntityDuplicateParams& params)
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), true));
			ENGINE_TRY_ASSIGN(const std::vector<Entity> roots, Utils::ResolveEntityRoots(context, *scene, params.Entities));

			SceneEdit edit(context.GetEditor(), Utils::MakeListLabel("Duplicate", roots));
			std::vector<UUID> copies;
			for (const Entity root : roots)
			{
				ENGINE_TRY_ASSIGN(const Entity copy, Utils::DuplicateSubtree(*scene, root));
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

	void RegisterEntityMethodTypes(TypeRegistry& registry)
	{
		constexpr std::string_view ComponentsDescription =
			"Component values by registry name, each an object of field values (missing fields keep their values); see component.schema.";
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

		registry.Struct<EntityUpdateParams>("EntityUpdateParams", "The params of entity.update: only the given members change.")
			.Field("entity", &EntityUpdateParams::Entity, "The entity: 16 hex digits, a unique id prefix of at least 6, or a path.")
			.Field("name", &EntityUpdateParams::Name, "The new name.")
			.Field("active", &EntityUpdateParams::Active, "The new active flag.")
			.Field("tags", &EntityUpdateParams::Tags, "The new tag list (replaces the old one).")
			.VariantField("components", &EntityUpdateParams::Components,
				"Component values by registry name, merged into the existing components; missing components are added.", &ResolveComponentValue)
			.Field("removeComponents", &EntityUpdateParams::RemoveComponents, "Components to remove (before the components are written).")
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

	void RegisterEntityMethods(MethodRegistry& methods)
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
				.Description = "Destroys entities with their subtrees, as one undoable command.",
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
