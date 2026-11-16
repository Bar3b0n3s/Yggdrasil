#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Input/KeyCodes.h"
#include "Engine/Platform/Window.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <type_traits>

namespace Engine {

	namespace Utils {

		template<typename Code, auto Query>
		static int InputButton(ScriptCall& call)
		{
			constexpr std::string_view Enumeration = std::is_same_v<Code, Key> ? "Key" : "MouseButton";
			const auto code = static_cast<Code>(Lua::CheckEnum(call, 1, Enumeration));
			const auto& host = call.Engine->GetHost();
			Lua::Push(call, (host.GetInput().*Query)(host.GetFrameState().Phase, code));
			return 1;
		}

		template<auto Query>
		static int InputMouseVector(ScriptCall& call)
		{
			const auto& host = call.Engine->GetHost();
			Lua::Push(call, (host.GetInput().*Query)(host.GetFrameState().Phase));
			return 1;
		}

		template<auto Member>
		static int InputAction(ScriptCall& call)
		{
			const auto name = Lua::Check<std::string>(call, 1);
			const auto action = call.Engine->GetHost().GetAction(name);
			if (!action)
				return Lua::RaiseError(call, action.error());
			Lua::Push(call, (*action).*Member);
			return 1;
		}

		static uint32_t InputGamepadIndex(ScriptCall& call)
		{
			const auto index = Lua::Check<uint32_t>(call, 1);
			if (index >= MaxGamepads)
				Lua::RaiseError(call, "gamepad index must be between 0 and 3");
			return index;
		}

		static int InputGamepadConnected(ScriptCall& call)
		{
			const auto index = InputGamepadIndex(call);
			const auto& host = call.Engine->GetHost();
			Lua::Push(call, host.GetInput().IsGamepadConnected(host.GetFrameState().Phase, index));
			return 1;
		}

		static int InputGamepadButton(ScriptCall& call)
		{
			const auto index = InputGamepadIndex(call);
			const auto button = static_cast<GamepadButton>(Lua::CheckEnum(call, 2, "GamepadButton"));
			const auto& host = call.Engine->GetHost();
			Lua::Push(call, host.GetInput().IsGamepadButtonDown(host.GetFrameState().Phase, index, button));
			return 1;
		}

		static int InputGamepadAxis(ScriptCall& call)
		{
			const auto index = InputGamepadIndex(call);
			const auto axis = static_cast<GamepadAxis>(Lua::CheckEnum(call, 2, "GamepadAxis"));
			const auto& host = call.Engine->GetHost();
			Lua::Push(call, host.GetInput().GetGamepadAxis(host.GetFrameState().Phase, index, axis));
			return 1;
		}

		static int InputSetCursor(ScriptCall& call)
		{
			const auto mode = static_cast<CursorMode>(Lua::CheckEnum(call, 1, "CursorMode"));
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			const auto result = call.Engine->GetHost().SetCursorMode(mode);
			if (!result)
				return Lua::RaiseError(call, result.error());
			return 0;
		}

		static int InputGetCursor(ScriptCall& call)
		{
			Lua::PushString(call, CursorModeToString(call.Engine->GetHost().GetCursorMode()));
			return 1;
		}

		// M22 (C22.3, Docs/Decisions/0033-m22-contract.md decision 46): the text latch of the current phase's view
		// (IScriptHost::GetText), empty in every host but the play session's (Lua::PushString pushes an empty view, the
		// default host's without a data pointer included, as "").
		static int InputGetText(ScriptCall& call)
		{
			const auto& host = call.Engine->GetHost();
			Lua::PushString(call, host.GetText(host.GetFrameState().Phase));
			return 1;
		}

		// M28 (Docs/Decisions/0043-m28-game-services.md decision 6): a frame-time signal. A decision taken on frame-paced
		// observations (Application.GetStats) is posted as input from the frame phase, which the next fixed step latches and
		// a recording records; a fixed-step gameplay callback acts directly instead, so posting there raises.
		static int InputPostSignal(ScriptCall& call)
		{
			const std::string name = Lua::Check<std::string>(call, 1);
			IScriptHost& host = call.Engine->GetHost();
			const bool gameplay = call.Execution != nullptr && call.Execution->Origin == ScriptExecutionOrigin::Gameplay;
			if (gameplay && host.GetFrameState().Phase == InputPhase::Step)
			{
				return Lua::RaiseError(call, "a signal records a decision taken on frame-paced observations, so it is posted from the frame phase "
											 "(OnUpdate, OnLateUpdate); a fixed-step callback decides deterministically and acts directly");
			}
			const Result<bool> posted = host.PostSignal(name);
			if (!posted)
				return Lua::RaiseError(call, posted.error());
			Lua::Push(call, *posted);
			return 1;
		}

		static int InputWasSignalled(ScriptCall& call)
		{
			const std::string name = Lua::Check<std::string>(call, 1);
			const IScriptHost& host = call.Engine->GetHost();
			Lua::Push(call, host.WasSignalled(host.GetFrameState().Phase, name));
			return 1;
		}

		template<typename Code, auto Name>
		static Status RegisterInputEnum(ScriptApiRegistry& api, std::string name, std::string description, size_t count)
		{
			EnumInfo enumeration(std::move(name), std::move(description));
			for (size_t i = 0; i < count; ++i)
			{
				const auto spelling = Name(static_cast<Code>(i));
				if (!spelling.empty())
					enumeration.AddEntry({ std::string(spelling), static_cast<int64_t>(i), std::format("The {} input value.", spelling) });
			}
			return api.RegisterEnum(enumeration);
		}

	}

	namespace ScriptBindings {

		Status RegisterInput(ScriptApiRegistry& api)
		{
			ENGINE_TRY((Utils::RegisterInputEnum<Key, KeyToString>(api, "Key", "Keyboard keys by their US-layout position.", KeyCodeCount)));
			ENGINE_TRY((Utils::RegisterInputEnum<MouseButton, MouseButtonToString>(api, "MouseButton", "Mouse button names.", MouseButtonCount)));
			ENGINE_TRY((Utils::RegisterInputEnum<GamepadButton, GamepadButtonToString>(api, "GamepadButton", "Gamepad button positions.", GamepadButtonCount)));
			ENGINE_TRY((Utils::RegisterInputEnum<GamepadAxis, GamepadAxisToString>(api, "GamepadAxis", "Up-positive stick axes and zero-to-one triggers.", GamepadAxisCount)));
			ENGINE_TRY((Utils::RegisterInputEnum<CursorMode, CursorModeToString>(api, "CursorMode", "Native cursor visibility and capture.", 3)));
			const ScriptMemberOptions read{ .Mutates = false };
			const ScriptMemberOptions key{ .Mutates = false, .EnumParameters = { { 1, "Key" } } };
			const ScriptMemberOptions mouse{ .Mutates = false, .EnumParameters = { { 1, "MouseButton" } } };
			api.Module("Input", "Queries the current step or frame latch, including injected input; gamepad indexes are zero-based.")
				.Function("IsKeyDown", &Utils::InputButton<Key, &InputState::IsKeyDown>, "(key: Key) -> boolean", "Returns the held key state in this phase.", key)
				.Function("IsKeyPressed", &Utils::InputButton<Key, &InputState::WasKeyPressed>, "(key: Key) -> boolean", "Returns this phase's press edge; repeated keys do not add an edge.", key)
				.Function("IsKeyReleased", &Utils::InputButton<Key, &InputState::WasKeyReleased>, "(key: Key) -> boolean", "Returns this phase's release edge.", key)
				.Function("IsMouseButtonDown", &Utils::InputButton<MouseButton, &InputState::IsMouseButtonDown>, "(button: MouseButton) -> boolean", "Returns the held mouse-button state.", mouse)
				.Function("IsMouseButtonPressed", &Utils::InputButton<MouseButton, &InputState::WasMouseButtonPressed>, "(button: MouseButton) -> boolean", "Returns this phase's mouse-button press edge.", mouse)
				.Function("IsMouseButtonReleased", &Utils::InputButton<MouseButton, &InputState::WasMouseButtonReleased>, "(button: MouseButton) -> boolean", "Returns this phase's mouse-button release edge.", mouse)
				.Function("GetMousePosition", &Utils::InputMouseVector<&InputState::GetMousePosition>, "() -> vector", "Returns the cursor position in view coordinates as vector(x, y, 0): the pixels of the session's view, origin top-left (the game view's image in ordinary play, the project's Window.Width x Window.Height in lockstep, recording, replay and test sessions).", read)
				.Function("GetMouseDelta", &Utils::InputMouseVector<&InputState::GetMouseDelta>, "() -> vector", "Returns accumulated cursor movement in this phase.", read)
				.Function("GetScrollDelta", &Utils::InputMouseVector<&InputState::GetScrollDelta>, "() -> vector", "Returns accumulated scroll, right-positive and up-positive.", read)
				.Function("IsActionDown", &Utils::InputAction<&ScriptActionState::Down>, "(name: string) -> boolean", "Returns a configured action's held state; an unknown name raises INPUT_UNKNOWN_ACTION.", read)
				.Function("IsActionPressed", &Utils::InputAction<&ScriptActionState::Pressed>, "(name: string) -> boolean",
					"Returns whether a configured action was pressed since the previous latch of the current phase: in OnFixedUpdate since the previous "
					"tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with "
					"IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises "
					"INPUT_UNKNOWN_ACTION.",
					read)
				.Function("IsActionReleased", &Utils::InputAction<&ScriptActionState::Released>, "(name: string) -> boolean",
					"Returns whether a configured action was released since the previous latch of the current phase: in OnFixedUpdate since the previous "
					"tick, in OnUpdate and OnLateUpdate since the previous frame. A press and release between two ticks reports both edges with "
					"IsActionDown false, and a frame that runs several ticks reports the edge on its first tick only; an unknown name raises "
					"INPUT_UNKNOWN_ACTION.",
					read)
				.Function("GetAxis", &Utils::InputAction<&ScriptActionState::Axis>, "(name: string) -> number", "Returns a configured action's combined axis value.", read)
				.Function("IsGamepadConnected", &Utils::InputGamepadConnected, "(index: number) -> boolean", "Returns whether gamepad 0 through 3 is connected.", read)
				.Function("IsGamepadButtonDown", &Utils::InputGamepadButton, "(index: number, button: GamepadButton) -> boolean", "Returns a gamepad button's held state.", { .Mutates = false, .EnumParameters = { { 2, "GamepadButton" } } })
				.Function("GetGamepadAxis", &Utils::InputGamepadAxis, "(index: number, axis: GamepadAxis) -> number", "Returns an engine-convention axis, or zero for a disconnected pad.", { .Mutates = false, .EnumParameters = { { 2, "GamepadAxis" } } })
				.Function("SetCursorMode", &Utils::InputSetCursor, "(mode: CursorMode) -> ()", "Changes cursor visibility/capture; refused during read-only evaluation.", { .EnumParameters = { { 1, "CursorMode" } } })
				.Function("GetCursorMode", &Utils::InputGetCursor, "() -> CursorMode", "Returns the canonical cursor mode name.", read)
				.Function("GetText", &Utils::InputGetText, "() -> string",
					"Returns the characters typed in the latched view as UTF-8: in the fixed step (OnFixedUpdate, tasks, physics and UI callbacks) "
					"the tick's committed characters, in the frame phase (OnUpdate, OnLateUpdate) those of every tick since the previous frame; "
					"empty when none were typed.",
					read)
				.Function("PostSignal", &Utils::InputPostSignal, "(name: string) -> boolean",
					"Posts the frame-time signal name (1 to 64 bytes of UTF-8) for the next fixed step, where Input.WasSignalled reads it: a "
					"decision taken on frame-paced observations (ending a run whose frame time stays over a budget) recorded as input, so "
					"input.record records it and a replay plays it back. Returns whether the session takes live signals: ordinary play and "
					"recordings do; replays, test runs and lockstep sessions ignore them (false) and take only played-back signals and those "
					"Test.InjectSignal or input.inject injects. A name already posted for the same tick counts once; a tick takes at most 64 "
					"distinct signals. A fixed-step gameplay callback raises an error: it decides deterministically and acts directly.")
				.Function("WasSignalled", &Utils::InputWasSignalled, "(name: string) -> boolean",
					"Returns whether the signal name was latched: in the fixed step (OnFixedUpdate, tasks, physics and UI callbacks) by this "
					"tick, in the frame phase (OnUpdate, OnLateUpdate) by any tick since the previous frame.",
					read);
			return {};
		}

	}

}
