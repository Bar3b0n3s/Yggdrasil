#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/EntityBounds.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/ScriptEngine.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>

namespace Engine {

	namespace Utils {

		static void EntityChecked(ScriptCall& call, Status status)
		{
			if (!status)
				Lua::RaiseError(call, status.error());
		}
		static Entity EntitySelf(ScriptCall& call)
		{
			if (!call.Engine)
				Lua::RaiseError(call, "requires an active script engine");
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			EntityChecked(call, ScriptProxy::ValidateEntity(call.Engine->GetHost(), identity));
			return call.Engine->GetHost().GetScene().FindEntityByID(identity.ID);
		}
		static void PushEntity(ScriptCall& call, Entity entity)
		{
			Lua::Push(call, ScriptEntityIdentity{ entity.IsValid() ? entity.GetUUID() : UUID{}, call.Engine->GetHost().GetSceneGeneration() });
		}
		static void EntityMutation(ScriptCall& call)
		{
			EntityChecked(call, call.PrepareHostMutation());
		}
		static std::string Tag(ScriptCall& call)
		{
			auto name = Lua::Check<std::string>(call, 2);
			if (name.empty() || !IsValidUtf8(name) || name.find('\0') != std::string::npos)
				Lua::RaiseError(call, "tag must be nonempty UTF-8 without NUL");
			return name;
		}
		static int EntityID(ScriptCall& call)
		{
			Lua::PushString(call, Lua::Check<ScriptEntityIdentity>(call, 1).ID.ToString());
			return 1;
		}
		static int EntityName(ScriptCall& call)
		{
			Lua::PushString(call, EntitySelf(call).GetName());
			return 1;
		}
		static int EntitySetName(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto name = Lua::Check<std::string>(call, 2);
			if (!IsValidUtf8(name) || name.find('\0') != std::string::npos)
				return Lua::RaiseError(call, "name must be UTF-8 without NUL");
			EntityMutation(call);
			entity.SetName(name);
			return 0;
		}
		static int EntityValid(ScriptCall& call)
		{
			Lua::Push(call, ScriptProxy::IsValid(call.Engine->GetHost(), Lua::Check<ScriptEntityIdentity>(call, 1)));
			return 1;
		}
		static int EntityActive(ScriptCall& call)
		{
			Lua::Push(call, EntitySelf(call).IsActive());
			return 1;
		}
		static int EntityActiveSelf(ScriptCall& call)
		{
			Lua::Push(call, EntitySelf(call).IsActiveSelf());
			return 1;
		}
		static int EntitySetActive(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const bool active = Lua::Check<bool>(call, 2);
			if (entity.IsActiveSelf() != active)
			{
				EntityMutation(call);
				entity.SetActive(active);
				EntityChecked(call, call.Engine->SynchronizeInstances());
			}
			return 0;
		}
		static int EntityHasTag(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			Lua::Push(call, entity.HasTag(Tag(call)));
			return 1;
		}
		static int EntityAddTag(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto tag = Tag(call);
			if (!entity.HasTag(tag))
			{
				EntityMutation(call);
				entity.AddTag(tag);
			}
			return 0;
		}
		static int EntityRemoveTag(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto tag = Tag(call);
			if (entity.HasTag(tag))
			{
				EntityMutation(call);
				entity.RemoveTag(tag);
			}
			return 0;
		}
		static int EntityTags(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			lua_newtable(call.State);
			int i = 1;
			for (const auto& tag : entity.GetTags())
			{
				Lua::PushString(call, tag);
				lua_rawseti(call.State, -2, i++);
			}
			return 1;
		}
		static int EntityComponent(ScriptCall& call)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			const auto name = Lua::Check<std::string>(call, 2);
			auto proxy = ScriptProxy::GetComponent(call.Engine->GetHost(), identity, name);
			if (!proxy)
				return Lua::RaiseError(call, proxy.error());
			if (*proxy)
				Lua::Push(call, **proxy);
			else
				Lua::PushNil(call);
			return 1;
		}
		static int EntityHasComponent(ScriptCall& call)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			const auto name = Lua::Check<std::string>(call, 2);
			auto proxy = ScriptProxy::GetComponent(call.Engine->GetHost(), identity, name);
			if (!proxy)
				return Lua::RaiseError(call, proxy.error());
			Lua::Push(call, proxy->has_value());
			return 1;
		}
		static const ComponentInfo& EntityComponentType(ScriptCall& call, std::string_view name)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			auto allowed = ScriptProxy::GetComponent(call.Engine->GetHost(), identity, name);
			if (!allowed)
				Lua::RaiseError(call, allowed.error());
			return *call.Engine->GetHost().GetTypes().FindComponent(name);
		}
		static int EntityAddComponent(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto name = Lua::Check<std::string>(call, 2);
			const auto& info = EntityComponentType(call, name);
			if (info.GetHostOps()->Has(entity))
				return Lua::RaiseError(call, "entity already has '" + name + "'");
			for (const auto* required : info.GetRequires())
				if (!required->GetHostOps()->Has(entity))
					return Lua::RaiseError(call, "component requires '" + required->GetName() + "'");
			for (const auto* excluded : info.GetExcludes())
				if (excluded->GetHostOps()->Has(entity))
					return Lua::RaiseError(call, "component excludes '" + excluded->GetName() + "'");
			if (info.HasFlag(ComponentFlags::UniquePerScene))
				for (UUID id : entity.GetScene()->GetCanonicalOrder())
				{
					const Entity other = entity.GetScene()->FindEntityByID(id);
					if (other.IsValid() && info.GetHostOps()->Has(other))
						return Lua::RaiseError(call, "component must be unique in the scene");
				}
			auto candidate = info.CreateDefault();
			if (!Lua::IsNoneOrNil(call, 3))
			{
				if (!lua_istable(call.State, 3))
					return Lua::RaiseError(call, "component values must be a table");
				lua_pushnil(call.State);
				while (lua_next(call.State, 3))
				{
					const auto key = Lua::Check<std::string>(call, -2);
					const auto* field = info.FindField(key);
					if (!field || !field->GetMeta().Scriptable || field->GetMeta().Hidden || field->IsReadOnly() || field->IsVirtual() || !HasFlag(field->GetMeta().Modes, call.Engine->GetRunMode()))
						return Lua::RaiseError(call, "field '" + key + "' cannot be initialized");
					lua_pop(call.State, 1);
				}
				ResolveContext resolve{ .Registry = &call.Engine->GetHost().GetTypes(), .Owner = candidate.get(), .OwnerType = &info, .Key = {}, .Schemas = call.Engine->GetHost().GetFieldSchemas() };
				for (const auto& field : info.GetFields())
				{
					lua_rawgetfield(call.State, 3, field->GetName().c_str());
					if (!lua_isnil(call.State, -1))
					{
						const auto value = Lua::CheckValueForOwner(call, -1, *field, resolve);
						EntityChecked(call, field->SetValue({ candidate.get(), nullptr }, value));
					}
					lua_pop(call.State, 1);
				}
			}
			auto json = info.ToJson(candidate.get());
			if (!json)
				return Lua::RaiseError(call, json.error());
			auto checked = info.CreateDefault();
			ReadContext context;
			context.Schemas = call.Engine->GetHost().GetFieldSchemas();
			context.Strict = true;
			EntityChecked(call, info.FromJson(checked.get(), JsonReader(*json), context));
			if (name == "Script")
				EntityChecked(call, Detail::ValidateBehaviourAssignment(call.Engine->GetHost(), static_cast<const ScriptComponent*>(checked.get())->Script.GetHandle()));
			EntityMutation(call);
			EntityChecked(call, ComponentAccess::AddComponent(entity, name, &*json, context.Schemas));
			if (name == "Script")
				EntityChecked(call, call.Engine->SynchronizeInstances());
			auto proxy = ScriptProxy::GetComponent(call.Engine->GetHost(), Lua::Check<ScriptEntityIdentity>(call, 1), name);
			if (!proxy)
				return Lua::RaiseError(call, proxy.error());
			if (*proxy)
				Lua::Push(call, **proxy);
			else
				Lua::PushNil(call);
			return 1;
		}
		static int EntityRemoveComponent(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto name = Lua::Check<std::string>(call, 2);
			const auto& info = EntityComponentType(call, name);
			if (!info.GetHostOps()->Has(entity))
				return Lua::RaiseError(call, "component is absent");
			if (!info.HasFlag(ComponentFlags::Removable))
				return Lua::RaiseError(call, "component cannot be removed");
			for (const auto* other : call.Engine->GetHost().GetTypes().GetComponents())
				if (other->GetHostOps() && other->GetHostOps()->Has(entity) && std::ranges::find(other->GetRequires(), &info) != other->GetRequires().end())
					return Lua::RaiseError(call, "component is required by '" + other->GetName() + "'");
			EntityMutation(call);
			EntityChecked(call, ComponentAccess::RemoveComponent(entity, name));
			return 0;
		}
		static int EntityScript(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			EntityChecked(call, Detail::ScriptEngineAccess::PushInstance(call, entity.GetUUID()));
			return 1;
		}
		static int EntityParent(ScriptCall& call)
		{
			PushEntity(call, EntitySelf(call).GetParent());
			return 1;
		}
		static int EntitySetParent(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			Entity parent;
			if (!Lua::IsNoneOrNil(call, 2))
			{
				const auto identity = Lua::Check<ScriptEntityIdentity>(call, 2);
				EntityChecked(call, ScriptProxy::ValidateEntity(call.Engine->GetHost(), identity));
				parent = entity.GetScene()->FindEntityByID(identity.ID);
			}
			const bool keepWorld = Lua::IsNoneOrNil(call, 3) || Lua::Check<bool>(call, 3);
			for (Entity ancestor = parent; ancestor.IsValid(); ancestor = ancestor.GetParent())
				if (ancestor == entity)
					return Lua::RaiseError(call, "an entity cannot become its own descendant");
			if (keepWorld && entity.GetParent() != parent)
			{
				auto local = TransformSystem::DecomposeMatrix(glm::affineInverse(parent ? TransformSystem::ComputeWorldMatrix(parent) : glm::mat4(1.0f)) * TransformSystem::ComputeWorldMatrix(entity));
				if (!local)
					return Lua::RaiseError(call, local.error());
			}
			EntityMutation(call);
			EntityChecked(call, entity.GetScene()->SetParent(entity, parent, {}, keepWorld));
			EntityChecked(call, call.Engine->SynchronizeInstances());
			return 0;
		}
		static int EntityChildren(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			lua_newtable(call.State);
			int i = 1;
			for (UUID id : entity.GetChildren())
			{
				const auto child = entity.GetScene()->FindEntityByID(id);
				if (child)
				{
					PushEntity(call, child);
					lua_rawseti(call.State, -2, i++);
				}
			}
			return 1;
		}
		static int EntityFindChild(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			const auto name = Lua::Check<std::string>(call, 2);
			const bool recursive = !Lua::IsNoneOrNil(call, 3) && Lua::Check<bool>(call, 3);
			std::vector<UUID> pending(entity.GetChildren().rbegin(), entity.GetChildren().rend());
			while (!pending.empty())
			{
				const UUID id = pending.back();
				pending.pop_back();
				const auto child = entity.GetScene()->FindEntityByID(id);
				if (!child)
					continue;
				if (child.GetName() == name)
				{
					PushEntity(call, child);
					return 1;
				}
				if (recursive)
					pending.insert(pending.end(), child.GetChildren().rbegin(), child.GetChildren().rend());
			}
			Lua::PushNil(call);
			return 1;
		}
		static int EntityBounds(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			auto* assets = call.Engine->GetHost().GetAssets();
			const auto bounds = assets ? ComputeEntityWorldBounds(entity, *assets, { .IncludeDescendants = false }) : std::nullopt;
			if (bounds)
			{
				Lua::Push(call, bounds->Min);
				Lua::Push(call, bounds->Max);
			}
			else
			{
				Lua::PushNil(call);
				Lua::PushNil(call);
			}
			return 2;
		}
		static int EntityDestroy(ScriptCall& call)
		{
			const Entity entity = EntitySelf(call);
			EntityMutation(call);
			entity.GetScene()->DestroyEntity(entity);
			return 0;
		}
		static int EntityEqual(ScriptCall& call)
		{
			const auto lhs = Lua::Check<ScriptEntityIdentity>(call, 1);
			const auto rhs = Lua::Check<ScriptEntityIdentity>(call, 2);
			Lua::Push(call, lhs.ID == rhs.ID);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterEntity(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions write{};
			api.Type("Entity", "A UUID and scene generation identifying an entity; no native address is exposed.")
				.Property("ID", Utils::EntityID, nullptr, "string", "The immutable 16-digit hexadecimal UUID.", read)
				.Property("Name", Utils::EntityName, Utils::EntitySetName, "string", "The entity name.", read)
				.Method("IsValid", Utils::EntityValid, "(self: Entity) -> boolean", "Whether this identity still names a live entity in this scene.", read)
				.Method("IsActive", Utils::EntityActive, "(self: Entity) -> boolean", "The effective active state including every ancestor.", read)
				.Method("IsActiveSelf", Utils::EntityActiveSelf, "(self: Entity) -> boolean", "The entity's local active state.", read)
				.Method("SetActive", Utils::EntitySetActive, "(self: Entity, active: boolean) -> ()", "Change local active state and dispatch effective lifecycle transitions before returning.", write)
				.Method("HasTag", Utils::EntityHasTag, "(self: Entity, tag: string) -> boolean", "Check an exact tag.", read)
				.Method("AddTag", Utils::EntityAddTag, "(self: Entity, tag: string) -> ()", "Add a nonempty tag if absent.", write)
				.Method("RemoveTag", Utils::EntityRemoveTag, "(self: Entity, tag: string) -> ()", "Remove a tag if present.", write)
				.Method("GetTags", Utils::EntityTags, "(self: Entity) -> {string}", "Copy tags in insertion order.", read)
				.Method("HasComponent", Utils::EntityHasComponent, "(self: Entity, name: string) -> boolean", "Check an exposed component by its exact registry name.", read)
				.Method("GetComponent", Utils::EntityComponent, "(self: Entity, name: string) -> any", "Get a component proxy, or nil when absent.", read)
				.Method("AddComponent", Utils::EntityAddComponent, "(self: Entity, name: string, values: {[string]: any}?) -> any", "Validate and add a component; Script instances are synchronized before returning.", write)
				.Method("RemoveComponent", Utils::EntityRemoveComponent, "(self: Entity, name: string) -> ()", "Remove a removable component when no present component requires it.", write)
				.Method("GetScript", Utils::EntityScript, "(self: Entity) -> any", "Get the existing behaviour instance or nil; cast to the module's exported instance type.", read)
				.Method("GetParent", Utils::EntityParent, "(self: Entity) -> Entity?", "Get the parent, or nil for a root.", read)
				.Method("SetParent", Utils::EntitySetParent, "(self: Entity, parent: Entity?, keepWorld: boolean?) -> ()", "Reparent to the last sibling, preserving world pose by default; synchronize effective lifecycle transitions.", write)
				.Method("GetChildren", Utils::EntityChildren, "(self: Entity) -> {Entity}", "Copy live children in sibling order.", read)
				.Method("FindChild", Utils::EntityFindChild, "(self: Entity, name: string, recursive: boolean?) -> Entity?", "Find the first exact child name in depth-first sibling order; recursive defaults false.", read)
				.Method("GetWorldBounds", Utils::EntityBounds, "(self: Entity) -> (vector?, vector?)", "Get this entity's render mesh world AABB; nil without a mesh.", read)
				.Method("Destroy", Utils::EntityDestroy, "(self: Entity) -> ()", "Mark this subtree for the normal destruction flush.", write)
				.Operator("__eq", Utils::EntityEqual, "(self: Entity, other: Entity) -> boolean", "Compare UUIDs; equality does not authorize access to an expired scene.", read);
			return {};
		}

	}

}
