#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

namespace Engine {

	namespace Utils {

		static int ApplicationQuit(ScriptCall& call)
		{
			const int32_t code = Lua::IsNoneOrNil(call, 1) ? 0 : Lua::Check<int32_t>(call, 1);
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			call.Engine->GetHost().RequestQuit(code);
			return 0;
		}

		static int ApplicationEditor(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsEditor);
			return 1;
		}

		static int ApplicationHeadless(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsHeadless);
			return 1;
		}

		static int ApplicationFocused(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().IsFocused);
			return 1;
		}

		static int ApplicationPlatform(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().Platform);
			return 1;
		}

		static int ApplicationVersion(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().Version);
			return 1;
		}

		static int ApplicationWindow(ScriptCall& call)
		{
			Lua::Push(call, call.Engine->GetHost().GetEnvironment().WindowSize);
			return 1;
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the performance readout, frame-paced like
		// ParticleEmitter:GetParticleCount: a fixed-step gameplay callback cannot read it, so it never reaches the state hash
		// through gameplay that the fixed step runs.
		static int ApplicationStats(ScriptCall& call)
		{
			const bool gameplay = call.Execution != nullptr && call.Execution->Origin == ScriptExecutionOrigin::Gameplay;
			if (gameplay && call.Engine->GetHost().GetFrameState().Phase == InputPhase::Step)
				return Lua::RaiseError(call, "the performance readout is frame-paced observation that the fixed step must not read, so a fixed-step "
											 "callback (OnFixedUpdate, a task, a physics, animation or UI event) cannot call Application.GetStats; "
											 "read it in OnUpdate or OnLateUpdate");
			const Result<ScriptPerformanceStats> read = call.Engine->GetHost().GetPerformanceStats();
			if (!read)
				return Lua::RaiseError(call, read.error());
			const ScriptPerformanceStats& stats = *read;
			lua_createtable(call.State, 0, 16);
			const auto field = [&call](const char* name, const auto& value)
			{
				Lua::Push(call, value);
				lua_setfield(call.State, -2, name);
			};
			field("RenderAvailable", stats.RenderAvailable);
			field("GpuAvailable", stats.GpuAvailable);
			field("Fps", stats.Fps);
			field("CpuMilliseconds", stats.CpuMilliseconds);
			field("GpuMilliseconds", stats.GpuMilliseconds);
			field("VisibleLights", stats.VisibleLights);
			field("DroppedLights", stats.DroppedLights);
			field("ShadowedLights", stats.ShadowedLights);
			field("DrawCalls", stats.DrawCalls);
			field("Triangles", static_cast<double>(stats.Triangles));
			field("Particles", stats.Particles);
			field("SkinnedMeshes", stats.SkinnedMeshes);
			field("Bodies", stats.Bodies);
			field("ActiveBodies", stats.ActiveBodies);
			field("Entities", stats.Entities);
			field("ScriptHeapBytes", static_cast<double>(stats.ScriptHeapBytes));
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterApplication(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			ENGINE_TRY(api.RegisterAlias("PerformanceStats",
				"{RenderAvailable: boolean, GpuAvailable: boolean, Fps: number, CpuMilliseconds: number, GpuMilliseconds: number, VisibleLights: number, "
				"DroppedLights: number, ShadowedLights: number, DrawCalls: number, Triangles: number, Particles: number, SkinnedMeshes: number, "
				"Bodies: number, ActiveBodies: number, Entities: number, ScriptHeapBytes: number}",
				"The performance readout of Application.GetStats: frame-paced observations, never simulation state."));
			api.Module("Application", "Application identity, window state and deferred quit requests.")
				.Function("Quit", &Utils::ApplicationQuit, "(exitCode: number?) -> ()", "Requests exit (default 0): stops editor Play, exits Runtime, or records quit and ends the current test suite. Never destroys the executing VM inline.")
				.Function("IsEditor", &Utils::ApplicationEditor, "() -> boolean",
					"True in the editor; false in the standalone Runtime. Gameplay that branches on it splits the editor's and the export's state "
					"hashes, which the games suite compares.",
					read)
				.Function("IsHeadless", &Utils::ApplicationHeadless, "() -> boolean", "Returns whether the host has no native window.", read)
				.Function("IsFocused", &Utils::ApplicationFocused, "() -> boolean",
					"Returns the current native-window focus state. Nondeterministic: it follows the host window in every session, lockstep, "
					"recording, replay and test sessions included, so never base gameplay on it.",
					read)
				.Function("GetPlatform", &Utils::ApplicationPlatform, "() -> string", "Returns the host platform name.", read)
				.Function("GetVersion", &Utils::ApplicationVersion, "() -> string", "Returns the running engine version.", read)
				.Function("GetWindowSize", &Utils::ApplicationWindow, "() -> vector", "Returns the size of the session's view in view coordinates as vector(width, height, 0): the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions.", read)
				.Function("GetStats", &Utils::ApplicationStats, "() -> PerformanceStats",
					"Returns the performance readout, in every configuration, Dist included: the frame loop's Fps and CPU frame time in "
					"milliseconds; the game view's last rendered frame (RenderAvailable; its GPU time summed over the passes with a GPU "
					"sample, GpuAvailable; the lights it shaded, VisibleLights at most the per-view cap, DroppedLights beyond it, "
					"ShadowedLights the shadowed spot and point lights; its DrawCalls and Triangles; SkinnedMeshes, the meshes with a skin "
					"it deformed), all 0 without a renderer; and the session's live Particles, its physics Bodies and ActiveBodies (both "
					"worlds), its Entities and the script heap in bytes. Frame-paced, like ParticleEmitter:GetParticleCount: a fixed-step "
					"gameplay callback (OnFixedUpdate, a task, a physics, animation or UI event) raises an error; read it in OnUpdate, "
					"OnLateUpdate or a test case, and never keep it in hashed state. An edit-scene evaluation, which has no play session, raises.",
					read);
			return {};
		}

	}

}
