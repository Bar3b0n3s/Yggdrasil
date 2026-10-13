#include "EnginePCH.h"
#include "Engine/Scripting/Private/ScriptEngineState.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Core/Assert.h"
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
#include "Engine/Scripting/Watchdog.h"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace Engine {

	Status ScriptEngine::State::CreateInstances(std::span<const UUID> entities)
	{
		if (entities.empty())
			return {};
		Scene& scene = Engine.GetHost().GetScene();
		std::vector<UUID> ordered;
		if (entities.size() == 1)
			ordered.push_back(entities.front());
		else
		{
			// Visit the requested entities in canonical order without copying the whole scene's order per creation.
			std::vector<UUID> selected(entities.begin(), entities.end());
			std::sort(selected.begin(), selected.end());
			for (const UUID id : scene.GetCanonicalOrder())
			{
				if (std::binary_search(selected.begin(), selected.end(), id))
					ordered.push_back(id);
			}
		}
		struct Created
		{
			int32_t Order = 0;
			UUID Entity{};
			uint64_t Serial = 0;
		};
		std::vector<Created> created;
		for (const UUID id : ordered)
		{
			if (Instances.contains(id))
				continue;
			const Entity entity = scene.FindEntityByID(id);
			if (!entity || !entity.HasComponent<ScriptComponent>() || !entity.GetComponent<ScriptComponent>().Script.IsValid())
				continue;
			// Every later entry of a faulted slice fails with the latched fault, so the remaining entities stay uncreated
			// until the next synchronization after recovery creates them.
			if (IsSliceFaulted())
				break;
			const Result<bool> result = CreateInstance(id);
			if (!result)
				Publish(result.error(), id, "OnCreate");
			else if (*result)
			{
				// Capture the identity before later user code runs: a module top can remove or replace any instance.
				const auto current = Instances.find(id);
				created.push_back({ current->second.Order, id, current->second.Serial });
			}
			if (Engine.IsStopped())
				break;
		}
		std::stable_sort(created.begin(), created.end(), [](const Created& left, const Created& right)
		{
			return left.Order < right.Order;
		});
		if (!Engine.IsReadOnly())
		{
			for (const Created& entry : created)
			{
				// Skip instances erased or recreated (with their own OnCreate) by an earlier callback in this batch.
				const auto current = Instances.find(entry.Entity);
				if (current == Instances.end() || current->second.Serial != entry.Serial || !scene.FindEntityByID(entry.Entity))
					continue;
				// An earlier OnCreate latched a memory or timeout fault: this one would be aborted at its first call and leave
				// a half-initialized instance with no error of its own, so it runs after recovery instead (M13 review F08).
				if (IsSliceFaulted())
				{
					current->second.CreateDeferred = true;
					HasDeferredCreates = true;
				}
				else
					Invoke(entry.Entity, "OnCreate", nullptr, true);
			}
		}
		return {};
	}

	bool ScriptEngine::State::IsSliceFaulted() const
	{
		if (Vm == nullptr || Detail::SandboxAccess::GetWatchdog(*Vm)->GetDepth() == 0)
			return false;
		const ScriptMemoryState memory = Vm->GetMemoryState();
		return PendingSafetyFailure.has_value() || memory.NeedsRecovery || memory.MustStop;
	}

	void ScriptEngine::State::InvokeDeferredCreates()
	{
		if (!HasDeferredCreates || Engine.IsStopped() || Engine.IsReadOnly() || IsSliceFaulted())
			return;
		HasDeferredCreates = false;
		RefreshLifecycleOrder();
		struct Deferred
		{
			int32_t Order = 0;
			UUID Entity{};
			uint64_t Serial = 0;
		};
		std::vector<Deferred> deferred;
		for (const UUID id : LifecycleOrder)
		{
			const auto found = Instances.find(id);
			if (found != Instances.end() && found->second.CreateDeferred)
				deferred.push_back({ found->second.Order, id, found->second.Serial });
		}
		// LifecycleOrder is canonical, so a stable sort gives (ExecutionOrder, canonical), the order of the original batch.
		std::stable_sort(deferred.begin(), deferred.end(), [](const Deferred& left, const Deferred& right)
		{
			return left.Order < right.Order;
		});
		Scene& scene = Engine.GetHost().GetScene();
		for (const Deferred& entry : deferred)
		{
			const auto current = Instances.find(entry.Entity);
			if (current == Instances.end() || current->second.Serial != entry.Serial || !current->second.CreateDeferred)
				continue;
			if (IsSliceFaulted())
			{
				// A deferred OnCreate latched a new fault; the rest wait for the next recovery again.
				HasDeferredCreates = true;
				return;
			}
			current->second.CreateDeferred = false;
			if (scene.FindEntityByID(entry.Entity))
				Invoke(entry.Entity, "OnCreate", nullptr, true);
		}
	}

	Result<bool> ScriptEngine::State::CreateInstance(UUID id)
	{
		const Entity entity = Engine.GetHost().GetScene().FindEntityByID(id);
		if (!entity || !entity.HasComponent<ScriptComponent>())
			return MakeError(ErrorCode::NotFound, "The script entity or component disappeared");
		const ScriptComponent component = entity.GetComponent<ScriptComponent>();
		Instance instance{};
		instance.Handle = component.Script.GetHandle();
		instance.Name = entity.GetName();
		instance.Order = component.ExecutionOrder;
		instance.Serial = NextInstanceSerial++;
		instance.Active = entity.IsActive();
		instance.Overrides = component.Fields;
		const uint64_t serial = instance.Serial;
		Instances.emplace(id, std::move(instance));
		LifecycleOrder.push_back(id);
		LifecycleDirty = true;
		AssetManager* assets = Engine.GetHost().GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::NotFound, "A script instance needs an asset manager");
		ENGINE_TRY_ASSIGN(auto asset, assets->Load(component.Script.GetHandle()));
		const auto script = AssetCast<ScriptData>(asset);
		if (script == nullptr || script->Kind != ScriptKind::Behaviour)
			return MakeError(ErrorCode::Validation, "SCRIPT_NOT_A_BEHAVIOUR: an entity needs a Behaviour script");
		Instances.find(id)->second.Script = script;
		ENGINE_TRY_ASSIGN(auto schemas, ScriptFieldSchemaSource::Create({ { component.Script.GetHandle(), script } }));
		const auto operation = [this, id, serial, script, schemas, component](ScriptCall& call) -> int
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
			// The module top ran user code that may have removed this instance, or replaced it with one that already
			// owns its table. Either way nothing remains to create; the unreferenced table is collected.
			const auto current = Instances.find(id);
			if (current != Instances.end() && current->second.Serial == serial)
				current->second.Table = lua_ref(call.State, table);
			return 0;
		};
		ENGINE_TRY_ASSIGN(auto result, RunNative(operation, ScriptExecutionOrigin::Gameplay, { .Entity = id, .Case = CurrentOwner.Case }, "OnCreate"));
		if (result.Failure)
		{
			Publish(*result.Failure, { .Entity = id, .Case = CurrentOwner.Case });
			return false;
		}
		// The module top may have removed this instance (or replaced it under the same entity) through the runtime API.
		const auto current = Instances.find(id);
		return current != Instances.end() && current->second.Serial == serial && current->second.Table != LUA_NOREF;
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
		if (callback == "OnCreate")
			found->second.Created = true;
		else if (!found->second.Created)
			return;
		// The table stays on the protected call's stack even when nested user code removes the instance.
		const int tableReference = found->second.Table;
		// Lifecycle callbacks are instance work (§11.3): their tasks belong to the instance and survive test-case
		// boundaries. The active case still receives their errors and test signals as attribution only.
		const ScriptTaskOwner owner{ .Entity = id, .Case = {} };
		const bool outerSignal = std::exchange(TestSignalRaised, false);
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
		// Nested callbacks consume their own signals, so an outer callback only sees a Fail/Skip it raised itself.
		const bool testSignal = std::exchange(TestSignalRaised, outerSignal);
		const bool safetyFailure = (!result && result.error().GetCode() == ErrorCode::Timeout)
			|| (result && result->Failure && (result->Failure->Kind == ScriptErrorKind::Timeout || result->Failure->Kind == ScriptErrorKind::Memory))
			|| (callbackFailure && (callbackFailure->Kind == ScriptErrorKind::Timeout || callbackFailure->Kind == ScriptErrorKind::Memory));
		// Fail/Skip raised by this callback is a test control outcome that ends the running case, never an instance fault.
		// Other failures publish normally even while the case is already terminal.
		if (testSignal && !safetyFailure)
			return;
		if (!result)
			Publish(result.error(), id, callback);
		else if (result->Failure)
			Publish(*result->Failure, owner);
		else if (callbackFailure)
			Publish(*callbackFailure, owner);
	}

	void ScriptEngine::State::InvokePhysics(const PhysicsEvent& event)
	{
		switch (event.Type)
		{
			case PhysicsEventType::CollisionEnter: Invoke(event.Self, "OnCollisionEnter", &event); return;
			case PhysicsEventType::CollisionExit:  Invoke(event.Self, "OnCollisionExit", &event); return;
			case PhysicsEventType::TriggerEnter:   Invoke(event.Self, "OnTriggerEnter", &event); return;
			case PhysicsEventType::TriggerExit:    Invoke(event.Self, "OnTriggerExit", &event); return;
		}
		ENGINE_CORE_ASSERT(false, "Unknown physics event");
	}

	void ScriptEngine::State::DeliverPendingPhysics()
	{
		if (std::none_of(Instances.begin(), Instances.end(), [](const auto& entry)
		{
			return !entry.second.PendingPhysics.empty();
		}))
			return;
		for (const UUID id : OrderedInstances(false))
		{
			const auto found = Instances.find(id);
			if (found == Instances.end() || !found->second.Started || found->second.PendingPhysics.empty())
				continue;
			const uint64_t serial = found->second.Serial;
			const std::vector<PhysicsEvent> events = std::exchange(found->second.PendingPhysics, {});
			for (const PhysicsEvent& event : events)
			{
				// A callback may remove, replace or fault this instance; its remaining held events are then discarded.
				const auto current = Instances.find(id);
				if (Engine.IsStopped() || current == Instances.end() || current->second.Serial != serial || current->second.FaultDisabled
					|| current->second.DestroyPrepared)
					break;
				InvokePhysics(event);
			}
		}
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
