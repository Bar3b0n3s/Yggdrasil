#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <chrono>

namespace Engine {

	namespace Utils {

		static int TimeDelta(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetFrameState().DeltaTime);
			return 1;
		}

		static int TimeFixedDelta(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetFrameState().FixedDeltaTime);
			return 1;
		}

		static int TimeSimulation(ScriptCall& call)
		{
			const auto frame = call.Engine->GetHost().GetFrameState();
			Lua::Push(call, static_cast<double>(frame.Tick) * frame.FixedDeltaTime);
			return 1;
		}

		static int TimeTick(ScriptCall& call)
		{
			Lua::Push(call, static_cast<double>(call.Engine->GetHost().GetFrameState().Tick));
			return 1;
		}

		static int TimeFrame(ScriptCall& call)
		{
			Lua::Push(call, static_cast<double>(call.Engine->GetHost().GetFrameState().Frame));
			return 1;
		}

		static int TimeScale(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetFrameState().TimeScale);
			return 1;
		}

		static int TimeSetScale(ScriptCall& call)
		{
			const double scale = Lua::Check<double>(call, 1);
			if (scale < 0.0 || scale > 100.0)
				return Lua::RaiseError(call, "time scale must be between 0 and 100");
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = call.Engine->GetHost().SetTimeScale(scale);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		static int TimeAlpha(ScriptCall& call)
		{
			const auto frame = call.Engine->GetHost().GetFrameState();
			Lua::Push(call, frame.Phase == InputPhase::Step ? 1.0f : frame.InterpolationAlpha);
			return 1;
		}

		static int TimeReal(ScriptCall& call)
		{
			const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
			Lua::Push(call, seconds);
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterTime(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Time", "Phase-aware simulation time; use fixed time for deterministic gameplay.")
				.Function("GetDeltaTime", &Utils::TimeDelta, "() -> number", "Returns scaled delta seconds for the current fixed or frame phase.", read)
				.Function("GetFixedDeltaTime", &Utils::TimeFixedDelta, "() -> number", "Returns the session's fixed step in seconds.", read)
				.Function("GetTime", &Utils::TimeSimulation, "() -> number", "Returns simulation seconds: tick multiplied by fixed delta.", read)
				.Function("GetTick", &Utils::TimeTick, "() -> number", "Returns the current simulation tick.", read)
				.Function("GetFrameCount", &Utils::TimeFrame, "() -> number", "Returns the current rendered-frame index.", read)
				.Function("GetTimeScale", &Utils::TimeScale, "() -> number", "Returns the current session time scale.", read)
				.Function("SetTimeScale", &Utils::TimeSetScale, "(scale: number) -> ()", "Sets the session time scale in [0, 100]; refused during read-only evaluation.")
				.Function("GetInterpolationAlpha", &Utils::TimeAlpha, "() -> number", "Returns one during the fixed phase, otherwise this frame's interpolation alpha.", read)
				.Function("GetRealTime", &Utils::TimeReal, "() -> number", "Returns monotonic wall-clock seconds from an unspecified epoch. Nondeterministic; never use for gameplay.", read);
			return {};
		}

	}

}
