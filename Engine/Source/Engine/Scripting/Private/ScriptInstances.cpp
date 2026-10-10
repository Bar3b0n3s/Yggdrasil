#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/BindingRegistration.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <algorithm>
#include <optional>

namespace Engine {

	Status ScriptEngine::State::CreateInstances(std::span<const UUID> entities)
	{
		RefreshLifecycleOrder();
		std::set<UUID> selected(entities.begin(), entities.end());
		std::vector<UUID> created;
		const auto canonical = Engine.GetHost().GetScene().GetCanonicalOrder();
		const std::vector<UUID> snapshot(canonical.begin(), canonical.end());
		for (const UUID id : snapshot)
		{
			if (!selected.contains(id) || Instances.contains(id))
				continue;
			const Entity entity = Engine.GetHost().GetScene().FindEntityByID(id);
			if (!entity || !entity.HasComponent<ScriptComponent>() || !entity.GetComponent<ScriptComponent>().Script.IsValid())
				continue;
			const Status result = CreateInstance(id);
			if (!result)
				Publish(result.error(), id, "OnCreate");
			else
				created.push_back(id);
			if (Engine.IsStopped())
				break;
		}
		std::stable_sort(created.begin(), created.end(), [this](UUID left, UUID right)
		{
			return Instances.find(left)->second.Order < Instances.find(right)->second.Order;
		});
		if (!Engine.IsReadOnly())
		{
			for (const UUID id : created)
			{
				if (Engine.GetHost().GetScene().FindEntityByID(id))
					Invoke(id, "OnCreate", nullptr, true);
			}
		}
		return {};
	}

	Status ScriptEngine::State::CreateInstance(UUID id)
	{
		const Entity entity = Engine.GetHost().GetScene().FindEntityByID(id);
		if (!entity || !entity.HasComponent<ScriptComponent>())
			return MakeError(ErrorCode::NotFound, "The script entity or component disappeared");
		const ScriptComponent component = entity.GetComponent<ScriptComponent>();
		Instance instance{};
		instance.Handle = component.Script.GetHandle();
		instance.Name = entity.GetName();
		instance.Order = component.ExecutionOrder;
		instance.Active = entity.IsActive();
		instance.Overrides = component.Fields;
		Instances.emplace(id, std::move(instance));
		AssetManager* assets = Engine.GetHost().GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::NotFound, "A script instance needs an asset manager");
		ENGINE_TRY_ASSIGN(auto asset, assets->Load(component.Script.GetHandle()));
		const auto script = AssetCast<ScriptData>(asset);
		if (script == nullptr || script->Kind != ScriptKind::Behaviour)
			return MakeError(ErrorCode::Validation, "SCRIPT_NOT_A_BEHAVIOUR: an entity needs a Behaviour script");
		Instances.find(id)->second.Script = script;
		ENGINE_TRY_ASSIGN(auto schemas, ScriptFieldSchemaSource::Create({ { component.Script.GetHandle(), script } }));
		const auto operation = [this, id, script, schemas, component](ScriptCall& call) -> int
		{
			const Status loaded = Detail::SandboxAccess::PushModule(call, *script);
			if (!loaded)
				return Lua::RaiseError(call, loaded.error());
			const int classIndex = lua_gettop(call.State);
			const auto registration = Detail::InspectScriptRegistration(call, classIndex);
			if (!registration || registration->Kind != ScriptKind::Behaviour || !lua_istable(call.State, classIndex))
				return Lua::RaiseError(call, "The compiled Behaviour did not return its registered class table");
			lua_pushvalue(call.State, classIndex);
			lua_setfield(call.State, classIndex, "__index");
			lua_createtable(call.State, 0, static_cast<int>(script->Fields.size() + 1));
			const int table = lua_gettop(call.State);
			Lua::Push(call, ScriptEntityIdentity{ id, Engine.GetHost().GetSceneGeneration() });
			lua_setfield(call.State, table, "Entity");
			ResolveContext resolve{};
			resolve.Registry = &Engine.GetHost().GetTypes();
			resolve.Schemas = schemas.get();
			for (const auto& schema : script->Fields)
			{
				const auto fieldResult = schemas->FindField(component.Script.GetHandle(), schema.Name);
				if (!fieldResult)
					return Lua::RaiseError(call, fieldResult.error());
				const FieldInfo& field = **fieldResult;
				auto value = ValueFromJson(JsonReader(schema.DefaultValue.Get()), field.GetType());
				if (!value)
					return Lua::RaiseError(call, value.error());
				const auto overridden = component.Fields.find(schema.Name);
				if (overridden != component.Fields.end())
				{
					auto overrideValue = ValueFromJson(JsonReader(overridden->second.Get()), field.GetType());
					ValidationContext validation;
					if (overrideValue)
						field.ValidateValue(*overrideValue, resolve, validation);
					if (overrideValue && !validation.HasErrors())
						value = std::move(overrideValue);
					else
						ENGINE_CORE_WARN("SCRIPT_FIELD_TYPE_MISMATCH: {}.{} uses its default", id, schema.Name);
				}
				Lua::PushValue(call, *value, field);
				lua_setfield(call.State, table, schema.Name.c_str());
			}
			for (const auto& [name, value] : component.Fields)
			{
				static_cast<void>(value);
				if (!schemas->FindField(component.Script.GetHandle(), name))
					ENGINE_CORE_WARN("SCRIPT_UNKNOWN_FIELD_OVERRIDE: {}.{} is preserved but not applied", id, name);
			}
			lua_pushvalue(call.State, classIndex);
			lua_setmetatable(call.State, table);
			const auto current = Instances.find(id);
			if (current == Instances.end())
				return Lua::RaiseError(call, "The script instance disappeared while loading its class");
			current->second.Table = lua_ref(call.State, table);
			return 0;
		};
		ENGINE_TRY_ASSIGN(auto result, RunNative(operation, ScriptExecutionOrigin::Gameplay, { .Entity = id, .Case = CurrentOwner.Case }, "OnCreate"));
		if (result.Failure)
			Publish(*result.Failure, { .Entity = id, .Case = CurrentOwner.Case });
		return {};
	}

	void ScriptEngine::State::Invoke(UUID id, std::string_view callback, const PhysicsEvent* event, bool destroying)
	{
		if (Engine.IsStopped() || Engine.IsReadOnly())
			return;
		const auto found = Instances.find(id);
		if (found == Instances.end() || found->second.FaultDisabled || found->second.Table == LUA_NOREF
			|| (!destroying && (found->second.DestroyPrepared || !found->second.Active)))
			return;
		const Entity entity = Engine.GetHost().GetScene().FindEntityByID(id);
		if (!destroying && (!entity || !entity.IsActive()))
			return;
		// The table stays on the protected call's stack even when nested user code removes the instance.
		const int tableReference = found->second.Table;
		const ScriptTaskOwner owner{ id, !CurrentOwner.Case.IsNull() ? CurrentOwner.Case : ActiveCase };
		std::optional<ScriptError> callbackFailure;
		const auto operation = [this, tableReference, callback, event, &callbackFailure](ScriptCall& call) -> int
		{
			lua_getref(call.State, tableReference);
			const int self = lua_gettop(call.State);
			const std::string name(callback);
			lua_getfield(call.State, self, name.c_str());
			if (lua_isnil(call.State, -1))
				return 0;
			if (!lua_isfunction(call.State, -1))
				return Lua::RaiseError(call, "A lifecycle callback must be a function or nil");
			const Status counted = Engine.GetApi().RecordCallback(call, "Behaviour", callback, Engine.GetRunMode());
			if (!counted)
				return Lua::RaiseError(call, counted.error());
			lua_pushvalue(call.State, self);
			int arguments = 1;
			if (callback == "OnFixedUpdate" || callback == "OnUpdate" || callback == "OnLateUpdate")
			{
				const ScriptFrameState frame = Engine.GetHost().GetFrameState();
				Lua::Push(call, callback == "OnFixedUpdate" ? frame.FixedDeltaTime : frame.DeltaTime);
				++arguments;
			}
			if (event != nullptr)
			{
				Lua::Push(call, ScriptEntityIdentity{ event->Other, Engine.GetHost().GetSceneGeneration() });
				++arguments;
				if (event->Type == PhysicsEventType::CollisionEnter)
				{
					lua_createtable(call.State, 0, 5);
					Lua::Push(call, event->Contact.Point);
					lua_setfield(call.State, -2, "Point");
					Lua::Push(call, event->Contact.Normal);
					lua_setfield(call.State, -2, "Normal");
					Lua::Push(call, event->Contact.RelativeSpeed);
					lua_setfield(call.State, -2, "RelativeSpeed");
					Lua::Push(call, ScriptEntityIdentity{ event->Contact.Collider, Engine.GetHost().GetSceneGeneration() });
					lua_setfield(call.State, -2, "Collider");
					Lua::Push(call, ScriptEntityIdentity{ event->Contact.OtherCollider, Engine.GetHost().GetSceneGeneration() });
					lua_setfield(call.State, -2, "OtherCollider");
					++arguments;
				}
			}
			const auto result = Lua::ProtectedCall(call, arguments, 0);
			if (!result)
				return Lua::RaiseError(call, result.error());
			callbackFailure = result->Failure;
			return 0;
		};
		const auto result = RunNative(operation, ScriptExecutionOrigin::Gameplay, owner, callback);
		const auto caseThread = References.find(owner.Case.Index);
		const bool terminalCase = !owner.Case.IsNull() && caseThread != References.end() && caseThread->second.Terminal.has_value();
		const bool safetyFailure = (!result && result.error().GetCode() == ErrorCode::Timeout)
			|| (result && result->Failure && (result->Failure->Kind == ScriptErrorKind::Timeout || result->Failure->Kind == ScriptErrorKind::Memory))
			|| (callbackFailure && (callbackFailure->Kind == ScriptErrorKind::Timeout || callbackFailure->Kind == ScriptErrorKind::Memory));
		// Fail/Skip is a test control outcome, including when a nested callback belongs to the running case.
		if (terminalCase && !safetyFailure)
			return;
		if (!result)
			Publish(result.error(), id, callback);
		else if (result->Failure)
			Publish(*result->Failure, owner);
		else if (callbackFailure)
			Publish(*callbackFailure, owner);
	}

	void ScriptEngine::State::Phase(std::string_view callback)
	{
		AssertOwner();
		if (Engine.IsStopped() || Engine.IsReadOnly())
			return;
		const Status synchronized = Engine.SynchronizeInstances();
		if (!synchronized)
		{
			Publish(synchronized.error());
			return;
		}
		const auto snapshot = OrderedInstances(true);
		for (const UUID id : snapshot)
			Invoke(id, callback);
	}

}
