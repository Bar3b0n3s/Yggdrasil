#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <lua.h>
#include <lualib.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
			lua_createtable(call.State, 0, 22);
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
			field("MaxShadedLights", stats.MaxShadedLights);
			field("CachedPointShadows", stats.CachedPointShadows);
			field("CachedSpotShadows", stats.CachedSpotShadows);
			field("SsrTiles", stats.SsrTiles);
			field("SsrTracedTiles", stats.SsrTracedTiles);
			field("SsrTracedPixels", stats.SsrTracedPixels);
			field("PlanarReflections", stats.PlanarReflections);
			field("DroppedPlanarReflections", stats.DroppedPlanarReflections);
			field("LodCrossFades", stats.LodCrossFades);
			field("DistanceCulledMeshes", stats.DistanceCulledMeshes);
			return 1;
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 3): the display settings. Where the calls do nothing (the
		// editor, headless runs, fixed-view sessions and an edit evaluation) GetDisplayModes is empty, GetDisplaySettings the
		// session's view as a window, and SetDisplaySettings returns false with the reason.
		static void PushDisplaySettings(ScriptCall& call, const DisplaySettings& settings)
		{
			lua_createtable(call.State, 0, 5);
			Lua::PushString(call, DisplayWindowModeToString(settings.WindowMode));
			lua_setfield(call.State, -2, "WindowMode");
			Lua::Push(call, settings.Width);
			lua_setfield(call.State, -2, "Width");
			Lua::Push(call, settings.Height);
			lua_setfield(call.State, -2, "Height");
			Lua::Push(call, settings.RefreshRate);
			lua_setfield(call.State, -2, "RefreshRate");
			Lua::Push(call, settings.VSync);
			lua_setfield(call.State, -2, "VSync");
		}

		// The host's settings, or for a host without a display (an edit evaluation) its view as a window, at least 1 x 1.
		static DisplaySettings ReadCurrentDisplaySettings(ScriptCall& call)
		{
			const IScriptHost& host = call.Engine->GetHost();
			if (const Result<DisplaySettings> settings = host.GetDisplaySettings(); settings.has_value())
				return *settings;
			const glm::vec2 view = host.GetEnvironment().WindowSize;
			return DisplaySettings{ .WindowMode = DisplayWindowMode::Windowed,
				.Width = std::max(1U, static_cast<uint32_t>(std::max(view.x, 0.0f))),
				.Height = std::max(1U, static_cast<uint32_t>(std::max(view.y, 0.0f))) };
		}

		static int ApplicationDisplayModes(ScriptCall& call)
		{
			const Result<std::vector<DisplayVideoMode>> modes = call.Engine->GetHost().GetDisplayModes();
			const size_t count = modes.has_value() ? modes->size() : 0;
			lua_createtable(call.State, static_cast<int>(count), 0);
			for (size_t index = 0; index < count; ++index)
			{
				const DisplayVideoMode& mode = (*modes)[index];
				lua_createtable(call.State, 0, 3);
				Lua::Push(call, mode.Width);
				lua_setfield(call.State, -2, "Width");
				Lua::Push(call, mode.Height);
				lua_setfield(call.State, -2, "Height");
				Lua::Push(call, mode.RefreshRate);
				lua_setfield(call.State, -2, "RefreshRate");
				lua_rawseti(call.State, -2, static_cast<int>(index + 1));
			}
			return 1;
		}

		static int ApplicationDisplaySettings(ScriptCall& call)
		{
			PushDisplaySettings(call, ReadCurrentDisplaySettings(call));
			return 1;
		}

		// The members of the settings table at index 1, read raw (no metamethods): a member left out (nil) keeps `value`; a
		// value of another type, a number that is not a whole number from 0 to 2^32 - 1, or a string that is no window mode
		// raises, naming the member.
		static void ReadDisplayNumber(ScriptCall& call, const char* name, uint32_t& value)
		{
			lua_rawgetfield(call.State, 1, name);
			if (!lua_isnil(call.State, -1))
			{
				if (lua_type(call.State, -1) != LUA_TNUMBER)
					Lua::RaiseError(call, std::format("settings.{} expected number, got {}", name, luaL_typename(call.State, -1)));
				const double number = lua_tonumber(call.State, -1);
				constexpr double Largest = static_cast<double>(std::numeric_limits<uint32_t>::max());
				if (!(number >= 0.0 && number <= Largest) || number != std::floor(number))
					Lua::RaiseError(call, std::format("settings.{} must be a whole number from 0 to {}", name, std::numeric_limits<uint32_t>::max()));
				value = static_cast<uint32_t>(number);
			}
			lua_pop(call.State, 1);
		}

		static void ReadDisplayVSync(ScriptCall& call, bool& value)
		{
			lua_rawgetfield(call.State, 1, "VSync");
			if (!lua_isnil(call.State, -1))
			{
				if (lua_type(call.State, -1) != LUA_TBOOLEAN)
					Lua::RaiseError(call, std::format("settings.VSync expected boolean, got {}", luaL_typename(call.State, -1)));
				value = lua_toboolean(call.State, -1) != 0;
			}
			lua_pop(call.State, 1);
		}

		// The longest text a WindowMode is compared as ("Borderless", the longest name, is 10 bytes): a longer string is no
		// mode, refused without copying it.
		constexpr size_t MaxDisplayWindowModeBytes = 32;

		static void ReadDisplayWindowMode(ScriptCall& call, DisplayWindowMode& value)
		{
			lua_rawgetfield(call.State, 1, "WindowMode");
			if (!lua_isnil(call.State, -1))
			{
				if (lua_type(call.State, -1) != LUA_TSTRING)
					Lua::RaiseError(call, std::format("settings.WindowMode expected string, got {}", luaL_typename(call.State, -1)));
				size_t length = 0;
				const char* text = lua_tolstring(call.State, -1, &length);
				const std::optional<DisplayWindowMode> mode =
					length <= MaxDisplayWindowModeBytes ? DisplayWindowModeFromString(std::string_view(text, length)) : std::nullopt;
				if (mode.has_value())
					value = *mode;
				else
				{
					const std::string shown = length <= MaxDisplayWindowModeBytes ? std::string(text, length) : std::format("a string of {} bytes", length);
					Lua::RaiseError(call, std::format("settings.WindowMode '{}' is not a window mode: Windowed, Borderless or Exclusive", shown));
				}
			}
			lua_pop(call.State, 1);
		}

		static int ApplicationSetDisplaySettings(ScriptCall& call)
		{
			if (!lua_istable(call.State, 1))
				return Lua::RaiseError(call, std::format("argument #1 expected table, got {}", luaL_typename(call.State, 1)));
			DisplaySettings settings = ReadCurrentDisplaySettings(call);
			ReadDisplayWindowMode(call, settings.WindowMode);
			ReadDisplayNumber(call, "Width", settings.Width);
			ReadDisplayNumber(call, "Height", settings.Height);
			ReadDisplayNumber(call, "RefreshRate", settings.RefreshRate);
			ReadDisplayVSync(call, settings.VSync);
			// The ranges hold wherever the call is made, so a request that would raise in the Runtime raises in the editor and
			// in a test run too; only the monitor's own list (an Exclusive mode it lacks) is the host's to check.
			if (const Status valid = ValidateDisplaySettings(settings); !valid)
				return Lua::RaiseError(call, valid.error());
			const Status requested = call.Engine->GetHost().RequestDisplaySettings(settings);
			if (!requested && requested.error().GetCode() != ErrorCode::Unsupported)
				return Lua::RaiseError(call, requested.error());
			Lua::Push(call, requested.has_value());
			if (requested.has_value())
				return 1;
			Lua::PushString(call, requested.error().GetMessageText());
			return 2;
		}

	}

	namespace ScriptBindings {

		Status RegisterApplication(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions read{ .Mutates = false };
			ENGINE_TRY(api.RegisterAlias("PerformanceStats",
				"{RenderAvailable: boolean, GpuAvailable: boolean, Fps: number, CpuMilliseconds: number, GpuMilliseconds: number, VisibleLights: number, "
				"DroppedLights: number, ShadowedLights: number, DrawCalls: number, Triangles: number, Particles: number, SkinnedMeshes: number, "
				"Bodies: number, ActiveBodies: number, Entities: number, ScriptHeapBytes: number, MaxShadedLights: number, CachedPointShadows: number, "
				"CachedSpotShadows: number, SsrTiles: number, SsrTracedTiles: number, SsrTracedPixels: number, PlanarReflections: number, "
				"DroppedPlanarReflections: number, LodCrossFades: number, DistanceCulledMeshes: number}",
				"The performance readout of Application.GetStats: frame-paced observations, never simulation state."));
			ENGINE_TRY(api.RegisterAlias("DisplayMode", "{Width: number, Height: number, RefreshRate: number}", "One video mode of the monitor: its size in pixels and refresh rate in Hz."));
			ENGINE_TRY(api.RegisterAlias("DisplaySettings", "{WindowMode: \"Windowed\" | \"Borderless\" | \"Exclusive\", Width: number, Height: number, RefreshRate: number, VSync: boolean}",
				"A window's display settings: WindowMode Windowed, Borderless or Exclusive; the window's size in screen units (the video mode's in the "
				"full-screen modes); the refresh rate of a full-screen mode (0 windowed); and vertical sync."));
			ENGINE_TRY(api.RegisterAlias("DisplaySettingsRequest", "{WindowMode: (\"Windowed\" | \"Borderless\" | \"Exclusive\")?, Width: number?, Height: number?, RefreshRate: number?, VSync: boolean?}",
				"A request of Application.SetDisplaySettings: the DisplaySettings members to change; a member left out keeps its current value."));
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
					"sample, GpuAvailable; the lights it shaded, VisibleLights at most MaxShadedLights, the cap it rendered with, "
					"DroppedLights beyond it, ShadowedLights the shadowed spot and point lights, CachedPointShadows and CachedSpotShadows "
					"those whose shadow maps it reused; its DrawCalls and Triangles; SkinnedMeshes, the meshes with a skin it deformed; and "
					"the area the SSR trace covered in the last completed frame that traced, SsrTiles, SsrTracedTiles and "
					"SsrTracedPixels; and PlanarReflections, the planar reflections it drew, DroppedPlanarReflections those past the cap of "
					"4 or whose GPU objects, or the view's probe constants, could not be created; and LodCrossFades, the mesh items it drew "
					"cross-fading two levels of detail, DistanceCulledMeshes those beyond their RenderDistance's CullDistance), all 0 "
					"without a renderer; and the session's live Particles, its physics Bodies and ActiveBodies "
					"(both worlds), its Entities and the script heap in bytes. Frame-paced, like ParticleEmitter:GetParticleCount: a fixed-step "
					"gameplay callback (OnFixedUpdate, a task, a physics, animation or UI event) raises an error; read it in OnUpdate, "
					"OnLateUpdate or a test case, and never keep it in hashed state. An edit-scene evaluation, which has no play session, raises.",
					read)
				.Function("GetDisplayModes", &Utils::ApplicationDisplayModes, "() -> {DisplayMode}",
					"Returns the video modes of the monitor the game's window is on, sorted by width, height and refresh rate: the resolutions "
					"Exclusive mode takes. Empty where display settings do nothing: the editor, headless runs, test runs, replays, recordings and "
					"lockstep sessions, and on Linux and macOS.",
					read)
				.Function("GetDisplaySettings", &Utils::ApplicationDisplaySettings, "() -> DisplaySettings",
					"Returns the display settings in force: the window mode, the window's size in screen units (the video mode's in the full-screen "
					"modes; its last size while the window is minimized), the refresh rate of a full-screen mode (0 windowed) and VSync. Where display settings do nothing it returns the "
					"session's view as a Windowed window with the project's Window.VSync.",
					read)
				.Function("SetDisplaySettings", &Utils::ApplicationSetDisplaySettings, "(settings: DisplaySettingsRequest) -> (boolean, string?)",
					"Requests display settings for the next frame boundary (a member left out keeps its value in force, so a later request in the "
					"same frame replaces an earlier one: give every member that should change): Windowed takes Width x "
					"Height; Borderless covers the monitor at its current mode; Exclusive switches the monitor to one of GetDisplayModes(). The "
					"Runtime keeps the choice in user://Display.json and applies it at the next start, before its window shows. Returns true when "
					"the request was accepted, or false and the reason where display settings do nothing (the editor, headless runs, test runs, "
					"replays, recordings, lockstep sessions, Linux and macOS), so they never touch the project's Window settings or the state "
					"hash. An unknown mode, a Width or Height outside 1 to 16384 or a RefreshRate over 1000 raises an error wherever the call is made, "
					"and in the Runtime so does an Exclusive mode the monitor does not list.");
			return {};
		}

	}

}
