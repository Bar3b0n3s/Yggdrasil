#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/WindComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/Wind.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/ScriptProxy.h"

// The Wind proxy's method (M46; Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3J): Wind:Sample, the
// wind's velocity at a world point and time (Scene/Wind.h's SampleWind), at the world's clock by default. Reads only: it
// works in read-only evaluations and on an entity pending destruction, and changes no state. Only IEEE-exact operations,
// floor and sqrt (Scripting is on the simulation path's lint scope, §4.12).

namespace Engine {

	namespace Utils {

		// The receiver of a wind method: a Wind that is live, or pending destruction until the flush (M20, W-P02).
		static Entity WindSelf(ScriptCall& call)
		{
			const auto proxy = Lua::Check<ScriptProxyIdentity>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			if (const Status valid = ScriptProxy::ValidateComponentForRead(host, proxy); !valid)
				Lua::RaiseError(call, valid.error());
			if (host.GetTypes().GetComponents()[proxy.ComponentTypeIndex]->GetName() != "Wind")
				Lua::RaiseError(call, "wrong component receiver");
			return host.GetScene().FindReadableEntityByID(proxy.Entity.ID);
		}

		// Wind:Sample(position, time): the wind's velocity in m/s at the world point `position` and the world time `time` in
		// seconds, which defaults to the world's clock (Time.GetTime: the world's tick times the fixed delta, held while the
		// world is paused).
		static int WindSample(ScriptCall& call)
		{
			const Entity entity = WindSelf(call);
			const glm::vec3 position = Lua::Check<glm::vec3>(call, 2);
			double time = 0.0;
			if (Lua::IsNoneOrNil(call, 3))
			{
				const ScriptFrameState frame = call.Engine->GetHost().GetFrameState();
				time = static_cast<double>(GetWorldTick(frame)) * frame.FixedDeltaTime;
			}
			else
			{
				time = Lua::Check<double>(call, 3);
			}
			const glm::dvec3 velocity = SampleWind(entity.GetComponent<WindComponent>(), glm::dvec3(position), time);
			Lua::Push(call, glm::vec3(velocity));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterWind(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Type("Wind").Method("Sample", Utils::WindSample, "(self: Wind, position: vector, time: number?) -> vector",
				"Read the wind's velocity, in metres per second, at the world point position and the world time in seconds (the world's clock, "
				"Time.GetTime, when time is nil): the steady Strength along the horizontal Direction plus the gusts, which travel with the wind, and "
				"Turbulence's sideways part. The same bits in every configuration, so gameplay may use it.",
				read);
			return {};
		}

	}

}
