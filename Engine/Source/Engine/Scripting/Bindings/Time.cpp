#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <chrono>
#include <utility>

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
			Lua::Push(call, static_cast<double>(GetWorldTick(frame)) * frame.FixedDeltaTime);
			return 1;
		}

		static int TimeTick(ScriptCall& call)
		{
			Lua::Push(call, static_cast<double>(GetWorldTick(call.Engine->GetHost().GetFrameState())));
			return 1;
		}

		static int TimeSessionTime(ScriptCall& call)
		{
			const auto frame = call.Engine->GetHost().GetFrameState();
			Lua::Push(call, static_cast<double>(frame.Tick) * frame.FixedDeltaTime);
			return 1;
		}

		static int TimeSessionTick(ScriptCall& call)
		{
			Lua::Push(call, static_cast<double>(call.Engine->GetHost().GetFrameState().Tick));
			return 1;
		}

		static int TimePauseWorld(ScriptCall& call)
		{
			const auto sounds = static_cast<WorldPauseSounds>(Lua::CheckEnum(call, 1, "WorldPauseSounds"));
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = call.Engine->GetHost().RequestWorldPause(true, sounds);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		static int TimeResumeWorld(ScriptCall& call)
		{
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = call.Engine->GetHost().RequestWorldPause(false, WorldPauseSounds::Pause);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		static int TimeIsWorldPaused(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().IsWorldPaused());
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
			// M35 (Docs/Decisions/0050-m35-pause-entity-state.md decision 3): the automation registry reflects the same
			// vocabulary for play.setWorldPaused (Automation/Methods/PlayMethods.cpp), which ScriptApiRegistry::Freeze accepts
			// as one enum while the names and values agree.
			EnumInfo sounds("WorldPauseSounds", "What a world pause does to the world's playing voices, those of every group but Music and Ui (today the Sfx group's); the Music and Ui groups' play on either way.");
			sounds.AddEntry({ "Pause", std::to_underlying(WorldPauseSounds::Pause), "The world's voices (every group's but Music and Ui) pause until the world resumes." });
			sounds.AddEntry({ "Finish", std::to_underlying(WorldPauseSounds::Finish), "The voices play on to their end." });
			ENGINE_TRY(api.RegisterEnum(sounds));
			const ScriptMemberOptions read{ .Mutates = false };
			api.Module("Time", "Phase-aware simulation time; use fixed time for deterministic gameplay.")
				.Function("GetDeltaTime", &Utils::TimeDelta, "() -> number", "Returns the fixed step in the fixed phase (the time scale changes how many steps a frame runs, not their length) and the scaled frame delta in seconds in the frame phase.", read)
				.Function("GetFixedDeltaTime", &Utils::TimeFixedDelta, "() -> number", "Returns the session's fixed step in seconds.", read)
				.Function("GetTime", &Utils::TimeSimulation, "() -> number", "Returns the world's simulation seconds: its tick multiplied by the fixed delta. It holds while the world is paused (Time.PauseWorld).", read)
				.Function("GetTick", &Utils::TimeTick, "() -> number", "Returns the world's current simulation tick, which holds while the world is paused (Time.PauseWorld); it equals Time.GetSessionTick in a session that never paused.", read)
				.Function("GetFrameCount", &Utils::TimeFrame, "() -> number", "Returns the current rendered-frame index. It counts rendered frames, not ticks, so it depends on the frame rate (one a tick in lockstep, any number in live play): for presentation and tests, never for gameplay or hashed state.", read)
				.Function("GetTimeScale", &Utils::TimeScale, "() -> number", "Returns the current session time scale.", read)
				.Function("SetTimeScale", &Utils::TimeSetScale, "(scale: number) -> ()", "Sets the session time scale in [0, 100]; refused during read-only evaluation. At 0 frames run without a fixed step, so an entity created meanwhile (in OnUpdate, OnLateUpdate or by an evaluation) gets no OnStart or OnUpdate until the first tick after the scale returns: spawn what must run while paused before setting the scale to 0. To pause a game while its menus run, pause its world instead (Time.PauseWorld), which keeps the ticks and the UI going.")
				.Function("GetInterpolationAlpha", &Utils::TimeAlpha, "() -> number", "Returns one during the fixed phase, otherwise this frame's interpolation alpha.", read)
				.Function("GetRealTime", &Utils::TimeReal, "() -> number", "Returns monotonic wall-clock seconds from an unspecified epoch. Nondeterministic; never use for gameplay.", read)
				// M35 (decisions 1 and 2): the world pause and the session's clock, appended in landing order.
				.Function("PauseWorld", &Utils::TimePauseWorld, "(sounds: WorldPauseSounds?) -> ()",
					"Pauses the world from the next fixed step on: physics and 2D physics, sprite and skeletal animation, particles and the native module's systems hold, and only behaviours whose class sets RunsWhileWorldPaused run OnFixedUpdate, OnUpdate, OnLateUpdate and tasks, while input, the UI, its OnUi callbacks, the start and destroy flushes and the transform updates run on. The world's clock (Time.GetTick, Time.GetTime) holds. sounds: Pause (the default) holds the world's voices, those of every group but Music and Ui (today the Sfx group's), Finish lets them play on; the Music and Ui groups play on either way, so music and stingers sound under a pause menu. Not the host's session pause (play.pause), which stops every tick. It is session state, like the time scale: it lasts across Scene.Load and Scene.Reload, so call Time.ResumeWorld before loading or restarting a scene (a level's restart), or the new scene starts with its world paused. Refused during read-only evaluation.",
					{ .EnumParameters = { { 1, "WorldPauseSounds", true, "Pause" } } })
				.Function("ResumeWorld", &Utils::TimeResumeWorld, "() -> ()", "Resumes the world from the next fixed step on, its bodies with the velocities they had, its held voices playing again. Refused during read-only evaluation.")
				.Function("IsWorldPaused", &Utils::TimeIsWorldPaused, "() -> boolean", "Returns whether the world is paused for the next fixed step: the latest Time.PauseWorld or Time.ResumeWorld, which takes effect then.", read)
				.Function("GetSessionTick", &Utils::TimeSessionTick, "() -> number", "Returns the session's current tick, which counts every fixed step, paused world or not: the clock of the behaviours that run while the world is paused.", read)
				.Function("GetSessionTime", &Utils::TimeSessionTime, "() -> number", "Returns the session's simulation seconds: its tick multiplied by the fixed delta, which counts on while the world is paused (Time.GetTime, the world's, holds).", read);
			return {};
		}

	}

}
