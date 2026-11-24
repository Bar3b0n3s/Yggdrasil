#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/WaterComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/WaterQueries.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <algorithm>
#include <optional>

// The water's script surface (M47, Docs/Decisions/0065-m47-water.md decision 3; Architecture §11.5, §9.12): the Water proxy's
// SampleHeight, SampleNormal, SampleVelocity and IsUnderwater, which read the receiver's own surface, and Physics.SampleWater,
// the rule for which water holds a point (Scene/WaterQueries.h), reopening the Physics module so Bindings/Physics.cpp stays
// as it was. Each takes an optional time in seconds of world time, by default the world's time now (Time.GetTime(): the
// world tick times the fixed delta, held by the world pause; 0 in an edit scene's evaluation), so a fixed-step script reads
// the surface the physics floats bodies on that tick. Reads only: they work in read-only evaluations and on an entity
// pending destruction, and change no state.

namespace Engine {

	namespace Utils {

		// The world's time of the host now: Time.GetTime()'s (Bindings/Time.cpp).
		static double WaterWorldTime(ScriptCall& call)
		{
			const ScriptFrameState frame = call.Engine->GetHost().GetFrameState();
			return static_cast<double>(frame.Tick - std::min(frame.WorldHeldTicks, frame.Tick)) * frame.FixedDeltaTime;
		}

		// The time of argument `index`: the world's time when absent or nil, else a finite number.
		static double WaterTimeArgument(ScriptCall& call, int index)
		{
			return Lua::IsNoneOrNil(call, index) ? WaterWorldTime(call) : Lua::Check<double>(call, index);
		}

		// The receiver of a water method: a Water that is live, or pending destruction until the flush (M20, W-P02).
		static ConstEntity WaterSelf(ScriptCall& call)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponentForRead(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "Water")
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindReadableEntityByID(proxy.Entity.ID);
		}

		// The receiver's own surface under the world point of argument 2 at the time of argument 3; nullopt outside a
		// Rectangle's footprint.
		static std::optional<SceneWaterSample> SampleOwnWater(ScriptCall& call)
		{
			const ConstEntity entity = WaterSelf(call);
			const glm::vec3 position = Lua::Check<glm::vec3>(call, 2);
			const double time = WaterTimeArgument(call, 3);
			return SampleWater(ResolveWater(entity), glm::dvec3(position), time);
		}

		// Water:SampleHeight(position, time?): the surface's world height under the point, nil outside a Rectangle's footprint.
		static int WaterSampleHeight(ScriptCall& call)
		{
			const std::optional<SceneWaterSample> sample = SampleOwnWater(call);
			if (!sample.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::Push(call, sample->Height);
			return 1;
		}

		// Water:SampleNormal(position, time?): the surface's world unit normal there, nil where SampleHeight is.
		static int WaterSampleNormal(ScriptCall& call)
		{
			const std::optional<SceneWaterSample> sample = SampleOwnWater(call);
			if (!sample.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::Push(call, glm::vec3(sample->Normal));
			return 1;
		}

		// Water:SampleVelocity(position, time?): the surface particle's velocity there plus the water's current, nil where
		// SampleHeight is.
		static int WaterSampleVelocity(ScriptCall& call)
		{
			const std::optional<SceneWaterSample> sample = SampleOwnWater(call);
			if (!sample.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			Lua::Push(call, glm::vec3(sample->Velocity));
			return 1;
		}

		// Water:IsUnderwater(position, time?): whether the point lies under this water's own surface (and within a
		// Rectangle's volume), masking aside.
		static int WaterIsUnderwater(ScriptCall& call)
		{
			const ConstEntity entity = WaterSelf(call);
			const glm::vec3 position = Lua::Check<glm::vec3>(call, 2);
			const double time = WaterTimeArgument(call, 3);
			Lua::Push(call, IsUnderWater(ResolveWater(entity), glm::dvec3(position), time));
			return 1;
		}

		// Physics.SampleWater(position, time?): the surface of the water that holds the point, by Scene/WaterQueries.h's rule,
		// as a WaterSample table; nil where no water does.
		static int PhysicsSampleWater(ScriptCall& call)
		{
			const glm::vec3 position = Lua::Check<glm::vec3>(call, 1);
			const double time = WaterTimeArgument(call, 2);
			IScriptHost& host = call.Engine->GetHost();
			const std::optional<SceneWaterSample> sample = SampleSceneWater(host.GetScene(), glm::dvec3(position), time);
			if (!sample.has_value())
			{
				Lua::PushNil(call);
				return 1;
			}
			lua_createtable(call.State, 0, 5);
			Lua::Push(call, ScriptEntityIdentity{ sample->Water, host.GetSceneGeneration() });
			lua_setfield(call.State, -2, "Water");
			Lua::Push(call, sample->Height);
			lua_setfield(call.State, -2, "Height");
			Lua::Push(call, glm::vec3(sample->Normal));
			lua_setfield(call.State, -2, "Normal");
			Lua::Push(call, glm::vec3(sample->Velocity));
			lua_setfield(call.State, -2, "Velocity");
			Lua::Push(call, sample->Depth);
			lua_setfield(call.State, -2, "Depth");
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterWater(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Type("Water")
				.Method("SampleHeight", Utils::WaterSampleHeight, "(self: Water, position: vector, time: number?) -> number?",
					"Read the world height of this water's own surface under the world point position at time seconds of world time (default "
					"Time.GetTime()), masking aside; nil outside a Rectangle's footprint.",
					read)
				.Method("SampleNormal", Utils::WaterSampleNormal, "(self: Water, position: vector, time: number?) -> vector?",
					"Read the world unit normal of this water's own surface under the world point position at time (default Time.GetTime()); nil "
					"where SampleHeight is nil.",
					read)
				.Method("SampleVelocity", Utils::WaterSampleVelocity, "(self: Water, position: vector, time: number?) -> vector?",
					"Read the velocity of this water's surface under the world point position at time (default Time.GetTime()): the waves' motion "
					"there plus the water's Current, in metres per second; nil where SampleHeight is nil.",
					read)
				.Method("IsUnderwater", Utils::WaterIsUnderwater, "(self: Water, position: vector, time: number?) -> boolean",
					"Whether the world point position lies under this water's own surface at time (default Time.GetTime()), and within a "
					"Rectangle's volume; other waters' masks aside.",
					read);
			ENGINE_TRY(api.RegisterAlias("WaterSample", "{Water: Entity, Height: number, Normal: vector, Velocity: vector, Depth: number}",
				"The surface of the water that holds a point: the Water's entity, the surface's world height, unit normal and velocity (with the "
				"current) under the point, and Depth, the height minus the point's y: positive under water."));
			api.Module("Physics", "Queries and gravity for the active scene's physics system.")
				.Function("SampleWater", Utils::PhysicsSampleWater, "(position: vector, time: number?) -> WaterSample?",
					"The surface of the water that holds the world point position at time seconds of world time (default Time.GetTime()): the "
					"Rectangle whose volume holds it with the highest surface there, else the scene's Ocean; nil where no water does. Inside a "
					"Rectangle's volume the Ocean is masked, so a cave above its lake is dry.",
					read);
			return {};
		}

	}

}
