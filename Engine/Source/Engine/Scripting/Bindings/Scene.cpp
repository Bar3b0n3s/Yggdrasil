#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentHostOps.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lua.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <vector>

namespace Engine {

	namespace Utils {

		static std::string SceneText(ScriptCall& call, int index)
		{
			auto value = Lua::Check<std::string>(call, index);
			if (!IsValidUtf8(value) || value.find('\0') != std::string::npos)
				Lua::RaiseError(call, "expected UTF-8 without embedded NULs");
			return value;
		}

		static UUID SceneParent(ScriptCall& call, int index)
		{
			if (Lua::IsNoneOrNil(call, index))
				return {};
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, index);
			const auto valid = ScriptProxy::ValidateEntity(call.Engine->GetHost(), identity);
			if (!valid)
				Lua::RaiseError(call, valid.error());
			return identity.ID;
		}

		static void ScenePushEntity(ScriptCall& call, UUID id)
		{
			auto& host = call.Engine->GetHost();
			if (!host.GetScene().FindEntityByID(id))
				Lua::PushNil(call);
			else
				Lua::Push(call, ScriptEntityIdentity{ id, host.GetSceneGeneration() });
		}

		static AssetHandle SceneAsset(ScriptCall& call, int index, AssetType type)
		{
			const auto handle = Lua::Check<AssetHandle>(call, index);
			auto* assets = call.Engine->GetHost().GetAssets();
			if (assets == nullptr)
				Lua::RaiseError(call, "asset service is unavailable");
			if (assets->GetAssetType(handle) != type)
				Lua::RaiseError(call, std::format("expected a registered {} asset", AssetTypeToString(type)));
			return handle;
		}

		static int SceneCreate(ScriptCall& call)
		{
			const auto name = Lua::IsNoneOrNil(call, 1) ? std::string("Entity") : SceneText(call, 1);
			const auto parent = SceneParent(call, 2);
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto created = call.Engine->GetHost().CreateEntity(name, parent);
			if (!created)
				return Lua::RaiseError(call, created.error());
			const auto notified = call.Engine->NotifyCreated(std::array{ *created });
			if (!notified)
				return Lua::RaiseError(call, notified.error());
			Lua::Push(call, ScriptEntityIdentity{ *created, call.Engine->GetHost().GetSceneGeneration() });
			return 1;
		}

		static int SceneInstantiate(ScriptCall& call)
		{
			const auto prefab = SceneAsset(call, 1, AssetType::Prefab);
			std::optional<glm::vec3> position;
			std::optional<glm::quat> rotation;
			if (!Lua::IsNoneOrNil(call, 2))
				position = Lua::Check<glm::vec3>(call, 2);
			if (!Lua::IsNoneOrNil(call, 3))
			{
				const auto value = Lua::Check<glm::quat>(call, 3);
				const glm::dquat wide(value.w, value.x, value.y, value.z);
				const auto unit = wide / std::sqrt(glm::dot(wide, wide));
				rotation = glm::quat(static_cast<float>(unit.w), static_cast<float>(unit.x), static_cast<float>(unit.y), static_cast<float>(unit.z));
			}
			const auto parent = SceneParent(call, 4);
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto root = call.Engine->GetHost().Instantiate(prefab, position, rotation, parent);
			if (!root)
				return Lua::RaiseError(call, root.error());
			// Snapshot the new subtree before OnCreate, which may make structural changes. Never retain component data.
			std::vector<UUID> created{ *root };
			for (size_t index = 0; index < created.size(); ++index)
			{
				const auto entity = call.Engine->GetHost().GetScene().FindEntityByID(created[index]);
				if (entity)
				{
					const auto children = entity.GetChildren();
					created.insert(created.end(), children.begin(), children.end());
				}
			}
			const auto notified = call.Engine->NotifyCreated(created);
			if (!notified)
				return Lua::RaiseError(call, notified.error());
			Lua::Push(call, ScriptEntityIdentity{ *root, call.Engine->GetHost().GetSceneGeneration() });
			return 1;
		}

		static int SceneDestroy(ScriptCall& call)
		{
			const auto identity = Lua::Check<ScriptEntityIdentity>(call, 1);
			auto& host = call.Engine->GetHost();
			const auto valid = ScriptProxy::ValidateEntity(host, identity);
			if (!valid)
				return Lua::RaiseError(call, valid.error());
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			host.GetScene().DestroyEntity(host.GetScene().FindEntityByID(identity.ID));
			return 0;
		}

		static int SceneFindID(ScriptCall& call)
		{
			const auto id = UUID::FromString(SceneText(call, 1));
			if (!id)
				return Lua::RaiseError(call, "id must contain exactly 16 hexadecimal digits");
			ScenePushEntity(call, *id);
			return 1;
		}

		template<bool Tags, bool All>
		static int SceneFind(ScriptCall& call)
		{
			const auto name = SceneText(call, 1);
			auto& host = call.Engine->GetHost();
			if constexpr (All)
				lua_newtable(call.State);
			for (const auto id : host.GetScene().GetCanonicalOrder())
			{
				const auto entity = host.GetScene().FindEntityByID(id);
				if (!entity || !(Tags ? entity.HasTag(name) : entity.GetName() == name))
					continue;
				Lua::Push(call, ScriptEntityIdentity{ id, host.GetSceneGeneration() });
				if constexpr (All)
					lua_rawseti(call.State, -2, lua_objlen(call.State, -2) + 1);
				else
					return 1;
			}
			if constexpr (!All)
				Lua::PushNil(call);
			return 1;
		}

		static int SceneFindPath(ScriptCall& call)
		{
			const auto path = SceneText(call, 1);
			const auto entity = call.Engine->GetHost().GetScene().FindEntityByPath(path);
			ScenePushEntity(call, entity ? entity.GetUUID() : UUID{});
			return 1;
		}

		static int SceneFindComponent(ScriptCall& call)
		{
			const auto name = SceneText(call, 1);
			auto& host = call.Engine->GetHost();
			const auto* component = host.GetTypes().FindComponent(name);
			if (component == nullptr || !component->HasFlag(ComponentFlags::ScriptVisible) || component->HasFlag(ComponentFlags::Hidden))
			{
				std::string message = std::format("unknown script-visible component '{}'", name);
				const auto suggestions = host.GetTypes().SuggestComponentNames(name);
				if (!suggestions.empty())
					message += std::format("; did you mean '{}' ?", suggestions.front());
				return Lua::RaiseError(call, message);
			}
			const auto* operations = component->GetHostOps();
			if (operations == nullptr || operations->Has == nullptr)
				return Lua::RaiseError(call, "component has no scene operations");
			lua_newtable(call.State);
			int index = 1;
			for (const auto id : host.GetScene().GetCanonicalOrder())
			{
				const auto entity = host.GetScene().FindEntityByID(id);
				if (entity && operations->Has(entity))
				{
					Lua::Push(call, ScriptEntityIdentity{ id, host.GetSceneGeneration() });
					lua_rawseti(call.State, -2, index++);
				}
			}
			return 1;
		}

		static int SceneCamera(ScriptCall& call)
		{
			auto& host = call.Engine->GetHost();
			for (const auto id : host.GetScene().GetCanonicalOrder())
			{
				const auto entity = host.GetScene().FindEntityByID(id);
				if (!entity || !entity.IsActive())
					continue;
				const auto* camera = entity.TryGetComponent<CameraComponent>();
				if (camera != nullptr && camera->Primary)
				{
					Lua::Push(call, ScriptEntityIdentity{ id, host.GetSceneGeneration() });
					return 1;
				}
			}
			Lua::PushNil(call);
			return 1;
		}

		static int SceneName(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetScene().GetName());
			return 1;
		}

		static int SceneCount(ScriptCall& call)
		{
			Lua::Push(call, static_cast<double>(call.Engine->GetHost().GetScene().GetEntityCount()));
			return 1;
		}

		static int SceneLoad(ScriptCall& call)
		{
			const auto asset = SceneAsset(call, 1, AssetType::Scene);
			auto parameters = Lua::IsNoneOrNil(call, 2) ? Json::object() : Lua::CheckJson(call, 2);
			if (!parameters.is_object())
				return Lua::RaiseError(call, "load parameters must be a JSON-serializable object table");
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = call.Engine->GetHost().RequestSceneLoad(asset, std::move(parameters));
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		static int SceneParameters(ScriptCall& call)
		{
			Lua::PushJson(call, call.Engine->GetHost().GetLoadParameters());
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterScene(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Scene", "Active-scene operations; searches return canonical order and never expose pending destruction.")
				.Function("CreateEntity", &Utils::SceneCreate, "(name: string?, parent: Entity?) -> Entity", "Creates immediately with a seeded ID and default name Entity. Runs OnCreate before returning; use IsValid if it may destroy itself.")
				.Function("Instantiate", &Utils::SceneInstantiate, "(prefab: AssetRef, position: vector?, rotation: Quat?, parent: Entity?) -> Entity", "Instantiates through the shared prefab path and invokes OnCreate for the complete subtree before returning.")
				.Function("Destroy", &Utils::SceneDestroy, "(entity: Entity) -> ()", "Marks the entity and descendants immediately; the session performs lifecycle teardown and destruction at its safe point.")
				.Function("FindByID", &Utils::SceneFindID, "(id: string) -> Entity?", "Finds an exact 16-digit hexadecimal UUID; malformed IDs error, absent entities return nil.", read)
				.Function("FindByName", &Utils::SceneFind<false, false>, "(name: string) -> Entity?", "Returns the first matching name in canonical scene order, or nil.", read)
				.Function("FindAllByName", &Utils::SceneFind<false, true>, "(name: string) -> {Entity}", "Returns all matching names in canonical scene order.", read)
				.Function("FindByPath", &Utils::SceneFindPath, "(path: string) -> Entity?", "Resolves an escaped absolute hierarchy path; missing, malformed or ambiguous paths return nil.", read)
				.Function("FindByTag", &Utils::SceneFind<true, false>, "(tag: string) -> Entity?", "Returns the first tagged entity in canonical scene order, or nil.", read)
				.Function("FindAllByTag", &Utils::SceneFind<true, true>, "(tag: string) -> {Entity}", "Returns all tagged entities in canonical scene order.", read)
				.Function("FindAllWithComponent", &Utils::SceneFindComponent, "(name: string) -> {Entity}", "Returns every entity carrying a script-visible component; unknown names raise a located error.", read)
				.Function("GetPrimaryCamera", &Utils::SceneCamera, "() -> Entity?", "Returns the first active Primary camera in canonical order, or nil.", read)
				.Function("GetName", &Utils::SceneName, "() -> string", "Returns the scene name.", read)
				.Function("GetEntityCount", &Utils::SceneCount, "() -> number", "Returns the number of live entities, excluding pending destruction.", read)
				.Function("Load", &Utils::SceneLoad, "(scene: AssetRef, parameters: {[string]: any}?) -> ()", "Requests a scene/VM replacement at the end of the frame with copied JSON parameters; never destroys the calling VM inline.")
				.Function("GetLoadParameters", &Utils::SceneParameters, "() -> {[string]: any}", "Returns a detached copy of the parameters passed to the current scene.", read);
			return {};
		}

	}

}
