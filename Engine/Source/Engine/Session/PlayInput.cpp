#include "EnginePCH.h"
#include "Engine/Session/PlayInput.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace Engine {

	namespace {

		constexpr size_t PhaseCount = 2; // InputPhase::Step and InputPhase::Frame

		// One project action's injected state (the action events of automation, replays and Test.InjectAction, §6.7), kept
		// like InputState keeps a control: the live level and value, the edges pending per phase since that phase's last
		// latch, and the latched view each phase's queries read.
		struct InjectedAction
		{
			struct View
			{
				bool Down = false;
				bool Pressed = false;
				bool Released = false;
				float Value = 0.0f;
			};

			bool Down = false;
			float Value = 0.0f;
			std::array<bool, PhaseCount> PendingPressed{};
			std::array<bool, PhaseCount> PendingReleased{};
			std::array<View, PhaseCount> Views{};
		};

	}

	namespace Utils {

		static size_t PhaseIndex(InputPhase phase)
		{
			return phase == InputPhase::Step ? 0 : 1;
		}

		static bool IsFinite(const glm::vec2& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y);
		}

		static bool IsTrigger(GamepadAxis axis)
		{
			return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
		}

		// True for a button use, whose State is read: an action without a value, a key, a mouse button or a gamepad button.
		static bool ReadsState(const PlayInputEvent& event)
		{
			switch (event.Type)
			{
				case PlayInputEventType::Action:             return !event.HasValue;
				case PlayInputEventType::KeyInput:           return true;
				case PlayInputEventType::MouseButtonInput:   return true;
				case PlayInputEventType::GamepadButtonInput: return true;
				case PlayInputEventType::MouseMove:
				case PlayInputEventType::MouseDelta:
				case PlayInputEventType::Scroll:
				case PlayInputEventType::GamepadAxisInput:
				case PlayInputEventType::Text:
					return false;
			}
			return false;
		}

		// InvalidArgument about the member `member` (a camelCase key of automation's InputEvent) of an event: located at
		// "/<member>" and carried as its one issue, so callers that know where the event sits rebase the pointer
		// (Automation::MakePlayInputEvent).
		static std::unexpected<Error> MakeMemberError(std::string_view member, std::string message, std::string hint = {})
		{
			const std::string pointer = std::format("/{}", member);
			ErrorLocation location;
			location.JsonPointer = pointer;
			ErrorIssue issue;
			issue.JsonPointer = pointer;
			issue.Message = message;
			issue.Hint = hint;
			return std::unexpected(
				Error(ErrorCode::InvalidArgument, std::move(message)).WithHint(std::move(hint)).WithLocation(std::move(location)).WithIssue(std::move(issue)));
		}

		// The project's Input.Actions (§6.1) as InputActionMap definitions, in the map's (byte-wise name) order.
		static std::vector<InputActionDefinition> MakeActionDefinitions(const InputSettings& settings)
		{
			std::vector<InputActionDefinition> definitions;
			definitions.reserve(settings.Actions.size());
			for (const auto& [name, action] : settings.Actions)
			{
				InputActionDefinition definition;
				definition.Name = name;
				definition.Type = action.Type == InputActionSettings::ActionType::Axis ? InputActionType::Axis : InputActionType::Button;
				definition.Bindings = action.Bindings;
				definition.Positive = action.Positive;
				definition.Negative = action.Negative;
				definition.Gamepad = action.Gamepad;
				definition.Invert = action.Invert;
				definitions.push_back(std::move(definition));
			}
			return definitions;
		}

		// The code points of `text`, which must be valid UTF-8 (asserted; ValidatePlayInputEvent checks events first).
		static std::vector<uint32_t> DecodeUtf8(std::string_view text)
		{
			ENGINE_CORE_ASSERT(IsValidUtf8(text), "DecodeUtf8 needs valid UTF-8");
			std::vector<uint32_t> codepoints;
			size_t index = 0;
			while (index < text.size())
			{
				const auto lead = static_cast<uint8_t>(text[index]);
				size_t length = 1;
				uint32_t codepoint = lead;
				if (lead >= 0xF0)
				{
					length = 4;
					codepoint = lead & 0x07u;
				}
				else if (lead >= 0xE0)
				{
					length = 3;
					codepoint = lead & 0x0Fu;
				}
				else if (lead >= 0xC0)
				{
					length = 2;
					codepoint = lead & 0x1Fu;
				}
				for (size_t continuation = 1; continuation < length && index + continuation < text.size(); ++continuation)
					codepoint = (codepoint << 6) | (static_cast<uint8_t>(text[index + continuation]) & 0x3Fu);
				codepoints.push_back(codepoint);
				index += length;
			}
			return codepoints;
		}

		// The UTF-8 encoding of a Unicode scalar value; empty for a surrogate or a value above U+10FFFF.
		static std::string EncodeUtf8(uint32_t codepoint)
		{
			std::string text;
			if ((codepoint >= 0xD800 && codepoint <= 0xDFFF) || codepoint > 0x10FFFF)
				return text;
			if (codepoint < 0x80)
			{
				text.push_back(static_cast<char>(codepoint));
			}
			else if (codepoint < 0x800)
			{
				text.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
				text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else if (codepoint < 0x10000)
			{
				text.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
				text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			else
			{
				text.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
				text.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
				text.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
			}
			return text;
		}

		// The binding name of a control ("Key.Space"), the names PlayInputSummary reports.
		static std::string MakeControlName(InputBinding::Control control)
		{
			return InputBinding{ .Target = control }.ToString();
		}

		// The value of `axis` with the largest magnitude over the connected gamepads (the lowest gamepad on a tie), as
		// InputActionMap reads gamepad axes.
		static float ReadLargestGamepadAxis(const InputState& devices, InputPhase phase, GamepadAxis axis)
		{
			float largest = 0.0f;
			for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
			{
				if (!devices.IsGamepadConnected(phase, gamepad))
					continue;
				const float value = devices.GetGamepadAxis(phase, gamepad, axis);
				if (std::abs(value) > std::abs(largest))
					largest = value;
			}
			return largest;
		}

	}

	struct PlayInput::State
	{
		InputState Devices;
		InputActionMap Actions;
		std::vector<InjectedAction> Injected; // parallel to the actions' indexes
		// The events not applied yet, by tick; each tick's list in queue order. Ordered, so iteration is canonical.
		std::map<uint64_t, std::vector<PlayInputEvent>> Queued;
		std::vector<PlayInputEvent> LastApplied;
		uint64_t NextTick = 0;
		// The live cursor and gamepad connections, which InputState reports only in its latched views: MouseDelta moves from
		// the live position, and injected gamepad events connect their gamepad first (see the class comment in the header).
		glm::vec2 MousePosition{ 0.0f };
		bool HasMousePosition = false;
		std::array<bool, MaxGamepads> GamepadConnected{};

		// Applies one queued event (Tap already expanded) to the devices or the injected actions.
		void Apply(const PlayInputEvent& event);
		// Sets the injected button level of action `action`, recording the edge for both phases.
		void SetInjectedButton(uint32_t action, bool down);
		// Moves the live cursor to `position` through a MouseMoveEvent.
		void MoveMouse(const glm::vec2& position);
		// Injects a Connected event for `gamepad` unless it is connected.
		void EnsureGamepadConnected(uint32_t gamepad);
		// Latches the injected actions' view of `phase`.
		void LatchActions(InputPhase phase);
	};

	void PlayInput::State::Apply(const PlayInputEvent& event)
	{
		switch (event.Type)
		{
			case PlayInputEventType::Action:
			{
				const std::optional<uint32_t> action = Actions.FindAction(event.Name);
				ENGINE_CORE_ASSERT(action.has_value(), "PlayInput: queued action event for the unknown action '{}'", event.Name);
				if (!action.has_value())
					return;
				if (event.HasValue)
					Injected[*action].Value = event.Value;
				else
					SetInjectedButton(*action, event.State == PlayInputEventState::Down);
				return;
			}
			case PlayInputEventType::KeyInput:
				Devices.Inject(KeyEvent{ .KeyCode = event.KeyCode,
					.Action = event.State == PlayInputEventState::Down ? ButtonAction::Pressed : ButtonAction::Released,
					.Modifiers = KeyModifiers::None,
					.Handled = false });
				return;
			case PlayInputEventType::MouseButtonInput:
				if (event.HasPosition)
					MoveMouse(event.Position);
				Devices.Inject(MouseButtonEvent{ .Button = event.Button,
					.Action = event.State == PlayInputEventState::Down ? ButtonAction::Pressed : ButtonAction::Released,
					.Modifiers = KeyModifiers::None,
					.Handled = false });
				return;
			case PlayInputEventType::MouseMove:
				MoveMouse(event.Position);
				return;
			case PlayInputEventType::MouseDelta:
				// The first MouseMoveEvent only sets InputState's cursor (no delta), so a delta as the first mouse input sets the
				// starting position first.
				if (!HasMousePosition)
					MoveMouse(MousePosition);
				MoveMouse(MousePosition + event.Delta);
				return;
			case PlayInputEventType::Scroll:
				Devices.Inject(MouseScrollEvent{ .Offset = event.Delta, .Handled = false });
				return;
			case PlayInputEventType::GamepadButtonInput:
				EnsureGamepadConnected(event.Gamepad);
				Devices.Inject(GamepadEvent{ .Gamepad = event.Gamepad,
					.Kind = GamepadEventKind::Button,
					.Button = event.GamepadButtonCode,
					.Pressed = event.State == PlayInputEventState::Down,
					.Axis = GamepadAxis::LeftX,
					.Value = 0.0f,
					.Handled = false });
				return;
			case PlayInputEventType::GamepadAxisInput:
				EnsureGamepadConnected(event.Gamepad);
				Devices.Inject(GamepadEvent{ .Gamepad = event.Gamepad,
					.Kind = GamepadEventKind::Axis,
					.Button = GamepadButton::South,
					.Pressed = false,
					.Axis = event.Axis,
					.Value = event.Value,
					.Handled = false });
				return;
			case PlayInputEventType::Text:
				for (const uint32_t codepoint : Utils::DecodeUtf8(event.Text))
					Devices.Inject(CharEvent{ .Codepoint = codepoint, .Handled = false });
				return;
		}
		ENGINE_CORE_ASSERT(false, "PlayInput: unknown PlayInputEventType {}", std::to_underlying(event.Type));
	}

	void PlayInput::State::SetInjectedButton(uint32_t action, bool down)
	{
		InjectedAction& injected = Injected[action];
		if (injected.Down == down)
			return;
		injected.Down = down;
		for (size_t phase = 0; phase < PhaseCount; ++phase)
		{
			if (down)
				injected.PendingPressed[phase] = true;
			else
				injected.PendingReleased[phase] = true;
		}
	}

	void PlayInput::State::MoveMouse(const glm::vec2& position)
	{
		Devices.Inject(MouseMoveEvent{ .Position = position, .Handled = false });
		// InputState ignores a position that is not finite; the live cursor stays where InputState keeps it.
		if (Utils::IsFinite(position))
		{
			MousePosition = position;
			HasMousePosition = true;
		}
	}

	void PlayInput::State::EnsureGamepadConnected(uint32_t gamepad)
	{
		if (gamepad >= MaxGamepads || GamepadConnected[gamepad])
			return;
		Devices.Inject(GamepadEvent{ .Gamepad = gamepad,
			.Kind = GamepadEventKind::Connected,
			.Button = GamepadButton::South,
			.Pressed = false,
			.Axis = GamepadAxis::LeftX,
			.Value = 0.0f,
			.Handled = false });
		GamepadConnected[gamepad] = true;
	}

	void PlayInput::State::LatchActions(InputPhase phase)
	{
		const size_t index = Utils::PhaseIndex(phase);
		for (InjectedAction& injected : Injected)
		{
			injected.Views[index] = InjectedAction::View{ .Down = injected.Down,
				.Pressed = injected.PendingPressed[index],
				.Released = injected.PendingReleased[index],
				.Value = injected.Value };
			injected.PendingPressed[index] = false;
			injected.PendingReleased[index] = false;
		}
	}

	Status ValidatePlayInputEvent(const PlayInputEvent& event, const InputActionMap& actions)
	{
		switch (event.Type)
		{
			case PlayInputEventType::Action:
			{
				if (!actions.FindAction(event.Name).has_value())
				{
					std::vector<std::string> names;
					names.reserve(actions.GetActionCount());
					for (uint32_t action = 0; action < actions.GetActionCount(); ++action)
						names.push_back(actions.GetDefinition(action).Name);
					const std::vector<std::string> suggestions = FuzzySuggest(event.Name, names);
					std::string hint = MakeDidYouMeanHint(suggestions);
					if (hint.empty())
					{
						hint = names.empty() ? std::string("the project defines no actions: add one to the project setting Input.Actions")
											 : std::string("name an action of the project setting Input.Actions (exact, case-sensitive)");
					}
					return Utils::MakeMemberError("name", std::format("unknown action '{}'", event.Name), std::move(hint));
				}
				if (event.HasValue)
				{
					if (event.State != PlayInputEventState::Down)
						return Utils::MakeMemberError("state", "an action event sets a state or a value, not both", "give either state or value");
					if (!std::isfinite(event.Value) || event.Value < -1.0f || event.Value > 1.0f)
						return Utils::MakeMemberError("value", std::format("the value of action '{}' must be in [-1, 1] (got {})", event.Name, event.Value));
				}
				return {};
			}
			case PlayInputEventType::KeyInput:
				if (KeyToString(event.KeyCode).empty())
					return Utils::MakeMemberError("key", std::format("{} is not a key", std::to_underlying(event.KeyCode)), "name a key such as \"Space\" or \"A\"");
				return {};
			case PlayInputEventType::MouseButtonInput:
				if (MouseButtonToString(event.Button).empty())
					return Utils::MakeMemberError("button", std::format("{} is not a mouse button", std::to_underlying(event.Button)));
				if (event.HasPosition && !Utils::IsFinite(event.Position))
					return Utils::MakeMemberError("position", "the position must be finite");
				return {};
			case PlayInputEventType::MouseMove:
				if (!Utils::IsFinite(event.Position))
					return Utils::MakeMemberError("position", "the position must be finite");
				return {};
			case PlayInputEventType::MouseDelta:
			case PlayInputEventType::Scroll:
				if (!Utils::IsFinite(event.Delta))
					return Utils::MakeMemberError("delta", "the delta must be finite");
				return {};
			case PlayInputEventType::GamepadButtonInput:
				if (event.Gamepad >= MaxGamepads)
					return Utils::MakeMemberError("gamepad", std::format("gamepad {} does not exist: gamepads are 0 to {}", event.Gamepad, MaxGamepads - 1));
				if (GamepadButtonToString(event.GamepadButtonCode).empty())
					return Utils::MakeMemberError("button", std::format("{} is not a gamepad button", std::to_underlying(event.GamepadButtonCode)));
				return {};
			case PlayInputEventType::GamepadAxisInput:
			{
				if (event.Gamepad >= MaxGamepads)
					return Utils::MakeMemberError("gamepad", std::format("gamepad {} does not exist: gamepads are 0 to {}", event.Gamepad, MaxGamepads - 1));
				if (GamepadAxisToString(event.Axis).empty())
					return Utils::MakeMemberError("axis", std::format("{} is not a gamepad axis", std::to_underlying(event.Axis)));
				const float minimum = Utils::IsTrigger(event.Axis) ? 0.0f : -1.0f;
				if (!std::isfinite(event.Value) || event.Value < minimum || event.Value > 1.0f)
				{
					return Utils::MakeMemberError("value",
						std::format("the value of gamepad axis {} must be in [{}, 1] (got {})", GamepadAxisToString(event.Axis), minimum, event.Value),
						Utils::IsTrigger(event.Axis) ? "triggers go from 0 (released) to 1 (fully pressed)" : "sticks go from -1 to 1, up and right positive");
				}
				return {};
			}
			case PlayInputEventType::Text:
				if (event.Text.empty())
					return Utils::MakeMemberError("text", "the text must hold at least one character");
				if (!IsValidUtf8(event.Text))
					return Utils::MakeMemberError("text", std::format("the text is not valid UTF-8 (at byte {})", FindInvalidUtf8(event.Text)));
				return {};
		}
		return Utils::MakeMemberError("type", std::format("{} is not an input event type", std::to_underlying(event.Type)));
	}

	PlayInput::PlayInput()
		: m_State(CreateScope<State>())
	{
	}

	PlayInput::~PlayInput() = default;
	PlayInput::PlayInput(PlayInput&&) noexcept = default;
	PlayInput& PlayInput::operator=(PlayInput&&) noexcept = default;

	Result<PlayInput> PlayInput::Create(const InputSettings& settings)
	{
		const std::vector<InputActionDefinition> definitions = Utils::MakeActionDefinitions(settings);
		Result<InputActionMap> actions = InputActionMap::Create(definitions);
		if (!actions)
		{
			// The map's pointers are "/<Name>/<Field>": they sit below the project's Input.Actions.
			std::vector<ErrorIssue> issues = actions.error().GetIssues();
			for (ErrorIssue& issue : issues)
				issue.JsonPointer = "/Input/Actions" + issue.JsonPointer;
			Error error(actions.error().GetCode(), actions.error().GetMessageText());
			ErrorLocation location;
			location.JsonPointer = "/Input/Actions";
			return std::unexpected(std::move(error).WithHint(actions.error().GetHint()).WithLocation(std::move(location)).WithIssues(std::move(issues)));
		}

		PlayInput input;
		input.m_State->Actions = std::move(*actions);
		input.m_State->Injected.resize(input.m_State->Actions.GetActionCount());
		return input;
	}

	Status PlayInput::Queue(uint64_t tick, const PlayInputEvent& event)
	{
		ENGINE_TRY(ValidatePlayInputEvent(event, m_State->Actions));
		if (tick < m_State->NextTick)
		{
			return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("tick {} already ran: the next tick is {}", tick, m_State->NextTick))
					.WithHint("stamp input for the next tick or a later one"));
		}

		if (event.State == PlayInputEventState::Tap && Utils::ReadsState(event))
		{
			// Tap is Down on its tick and Up on the next (§13.6), so both edges are observed.
			PlayInputEvent down = event;
			down.State = PlayInputEventState::Down;
			PlayInputEvent up = event;
			up.State = PlayInputEventState::Up;
			m_State->Queued[tick].push_back(std::move(down));
			m_State->Queued[tick + 1].push_back(std::move(up));
			return {};
		}
		m_State->Queued[tick].push_back(event);
		return {};
	}

	void PlayInput::QueueReleaseAll(uint64_t tick)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(tick >= state.NextTick, "PlayInput::QueueReleaseAll at tick {}, which already ran (the next tick is {})", tick,
			state.NextTick);

		// The latest queued state: the live state with every queued event applied, on a scratch copy.
		State projected;
		projected.Devices = state.Devices;
		projected.Actions = state.Actions;
		projected.Injected = state.Injected;
		projected.MousePosition = state.MousePosition;
		projected.HasMousePosition = state.HasMousePosition;
		projected.GamepadConnected = state.GamepadConnected;
		for (const auto& [queuedTick, events] : state.Queued)
		{
			for (const PlayInputEvent& event : events)
				projected.Apply(event);
		}
		projected.Devices.LatchStep();

		std::vector<PlayInputEvent> releases;
		for (size_t code = 0; code < KeyCodeCount; ++code)
		{
			const auto key = static_cast<Key>(code);
			if (!KeyToString(key).empty() && projected.Devices.IsKeyDown(InputPhase::Step, key))
				releases.push_back(PlayInputEvent{ .Type = PlayInputEventType::KeyInput, .State = PlayInputEventState::Up, .KeyCode = key });
		}
		for (size_t code = 0; code < MouseButtonCount; ++code)
		{
			const auto button = static_cast<MouseButton>(code);
			if (projected.Devices.IsMouseButtonDown(InputPhase::Step, button))
				releases.push_back(PlayInputEvent{ .Type = PlayInputEventType::MouseButtonInput, .State = PlayInputEventState::Up, .Button = button });
		}
		for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
		{
			if (!projected.Devices.IsGamepadConnected(InputPhase::Step, gamepad))
				continue;
			for (size_t code = 0; code < GamepadButtonCount; ++code)
			{
				const auto button = static_cast<GamepadButton>(code);
				if (projected.Devices.IsGamepadButtonDown(InputPhase::Step, gamepad, button))
				{
					releases.push_back(PlayInputEvent{
						.Type = PlayInputEventType::GamepadButtonInput, .State = PlayInputEventState::Up, .Gamepad = gamepad, .GamepadButtonCode = button });
				}
			}
			for (size_t code = 0; code < GamepadAxisCount; ++code)
			{
				const auto axis = static_cast<GamepadAxis>(code);
				if (projected.Devices.GetGamepadAxis(InputPhase::Step, gamepad, axis) != 0.0f)
				{
					releases.push_back(
						PlayInputEvent{ .Type = PlayInputEventType::GamepadAxisInput, .Value = 0.0f, .Gamepad = gamepad, .Axis = axis });
				}
			}
		}
		for (uint32_t action = 0; action < projected.Actions.GetActionCount(); ++action)
		{
			const InjectedAction& injected = projected.Injected[action];
			const std::string& name = projected.Actions.GetDefinition(action).Name;
			if (injected.Down)
				releases.push_back(PlayInputEvent{ .Type = PlayInputEventType::Action, .Name = name, .State = PlayInputEventState::Up });
			if (injected.Value != 0.0f)
				releases.push_back(PlayInputEvent{ .Type = PlayInputEventType::Action, .Name = name, .HasValue = true, .Value = 0.0f });
		}

		std::vector<PlayInputEvent>& events = state.Queued[tick];
		events.insert(events.begin(), std::make_move_iterator(releases.begin()), std::make_move_iterator(releases.end()));
		if (events.empty())
			state.Queued.erase(tick);
	}

	void PlayInput::QueueDeviceEvent(const Event& event)
	{
		State& state = *m_State;
		std::optional<PlayInputEvent> converted;
		std::visit(Overloaded{
					   [&converted](const KeyEvent& key)
		{
			// Key repeats change no input state (InputState ignores them) and are no game input.
			if (key.Action == ButtonAction::Repeated)
				return;
			converted = PlayInputEvent{ .Type = PlayInputEventType::KeyInput,
				.State = key.Action == ButtonAction::Pressed ? PlayInputEventState::Down : PlayInputEventState::Up,
				.KeyCode = key.KeyCode };
		},
					   [&converted](const MouseButtonEvent& button)
		{
			converted = PlayInputEvent{ .Type = PlayInputEventType::MouseButtonInput,
				.State = button.Action == ButtonAction::Released ? PlayInputEventState::Up : PlayInputEventState::Down,
				.Button = button.Button };
		},
					   [&converted](const MouseMoveEvent& move)
		{
			converted = PlayInputEvent{ .Type = PlayInputEventType::MouseMove, .Position = move.Position };
		},
					   [&converted](const MouseScrollEvent& scroll)
		{
			converted = PlayInputEvent{ .Type = PlayInputEventType::Scroll, .Delta = scroll.Offset };
		},
					   [&converted, &state](const GamepadEvent& gamepad)
		{
			switch (gamepad.Kind)
			{
				case GamepadEventKind::Connected:
				case GamepadEventKind::Disconnected:
					// Connection changes are no replayable input: they apply at once.
					if (gamepad.Gamepad < MaxGamepads)
					{
						state.Devices.Inject(gamepad);
						state.GamepadConnected[gamepad.Gamepad] = gamepad.Kind == GamepadEventKind::Connected;
					}
					return;
				case GamepadEventKind::Button:
					converted = PlayInputEvent{ .Type = PlayInputEventType::GamepadButtonInput,
						.State = gamepad.Pressed ? PlayInputEventState::Down : PlayInputEventState::Up,
						.Gamepad = gamepad.Gamepad,
						.GamepadButtonCode = gamepad.Button };
					return;
				case GamepadEventKind::Axis:
					converted = PlayInputEvent{ .Type = PlayInputEventType::GamepadAxisInput, .Value = gamepad.Value, .Gamepad = gamepad.Gamepad, .Axis = gamepad.Axis };
					return;
			}
		},
					   [&converted](const CharEvent& character)
		{
			std::string text = Utils::EncodeUtf8(character.Codepoint);
			if (!text.empty())
				converted = PlayInputEvent{ .Type = PlayInputEventType::Text, .Text = std::move(text) };
		},
					   [](const auto& /*other*/)
		{
			// Window and file-drop events are no game input.
		} },
			event);

		// A device event outside the ranges the queue accepts (a key GLFW does not name, a stray gamepad index) is no input
		// the game can read, so it is dropped like InputState drops it.
		if (!converted.has_value() || !ValidatePlayInputEvent(*converted, state.Actions).has_value())
			return;

		// Continuous sources are coalesced with what is queued for the same tick (see the header).
		std::vector<PlayInputEvent>& events = state.Queued[state.NextTick];
		if (converted->Type == PlayInputEventType::Scroll)
		{
			const auto previous = std::ranges::find_if(events.rbegin(), events.rend(), [](const PlayInputEvent& queued)
			{
				return queued.Type == PlayInputEventType::Scroll;
			});
			if (previous != events.rend())
			{
				previous->Delta += converted->Delta;
				return;
			}
		}
		else if (converted->Type == PlayInputEventType::MouseMove || converted->Type == PlayInputEventType::GamepadAxisInput)
		{
			const PlayInputEvent& latest = *converted;
			std::erase_if(events, [&latest](const PlayInputEvent& queued)
			{
				if (queued.Type != latest.Type)
					return false;
				return latest.Type == PlayInputEventType::MouseMove || (queued.Gamepad == latest.Gamepad && queued.Axis == latest.Axis);
			});
		}
		events.push_back(std::move(*converted));
	}

	void PlayInput::ApplyTick(uint64_t tick)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(tick == state.NextTick, "PlayInput::ApplyTick({}) out of order: the next tick is {}", tick, state.NextTick);
		state.LastApplied.clear();
		if (const auto queued = state.Queued.find(tick); queued != state.Queued.end())
		{
			state.LastApplied = std::move(queued->second);
			state.Queued.erase(queued);
		}
		for (const PlayInputEvent& event : state.LastApplied)
			state.Apply(event);
		state.Devices.LatchStep();
		state.LatchActions(InputPhase::Step);
		state.NextTick = tick + 1;
	}

	void PlayInput::LatchFrame()
	{
		m_State->Devices.LatchFrame();
		m_State->LatchActions(InputPhase::Frame);
	}

	uint64_t PlayInput::GetNextTick() const
	{
		return m_State->NextTick;
	}

	size_t PlayInput::GetQueuedEventCount() const
	{
		size_t count = 0;
		for (const auto& [tick, events] : m_State->Queued)
			count += events.size();
		return count;
	}

	std::span<const PlayInputEvent> PlayInput::GetLastAppliedEvents() const
	{
		return m_State->LastApplied;
	}

	const InputState& PlayInput::GetDevices() const
	{
		return m_State->Devices;
	}

	const InputActionMap& PlayInput::GetActions() const
	{
		return m_State->Actions;
	}

	bool PlayInput::IsActionDown(InputPhase phase, uint32_t action) const
	{
		ENGINE_CORE_ASSERT(action < m_State->Actions.GetActionCount(), "PlayInput: action {} does not exist", action);
		return m_State->Actions.IsDown(m_State->Devices, phase, action) || m_State->Injected[action].Views[Utils::PhaseIndex(phase)].Down;
	}

	bool PlayInput::WasActionPressed(InputPhase phase, uint32_t action) const
	{
		ENGINE_CORE_ASSERT(action < m_State->Actions.GetActionCount(), "PlayInput: action {} does not exist", action);
		return m_State->Actions.WasPressed(m_State->Devices, phase, action) || m_State->Injected[action].Views[Utils::PhaseIndex(phase)].Pressed;
	}

	bool PlayInput::WasActionReleased(InputPhase phase, uint32_t action) const
	{
		ENGINE_CORE_ASSERT(action < m_State->Actions.GetActionCount(), "PlayInput: action {} does not exist", action);
		return m_State->Actions.WasReleased(m_State->Devices, phase, action) || m_State->Injected[action].Views[Utils::PhaseIndex(phase)].Released;
	}

	float PlayInput::GetActionAxis(InputPhase phase, uint32_t action) const
	{
		ENGINE_CORE_ASSERT(action < m_State->Actions.GetActionCount(), "PlayInput: action {} does not exist", action);
		const InputActionMap& actions = m_State->Actions;
		// A Button action's axis is 1 while it is down (InputActionMap's rule), an injected button use counting as a binding.
		const float bound = actions.GetDefinition(action).Type == InputActionType::Button ? (IsActionDown(phase, action) ? 1.0f : 0.0f)
																						  : actions.GetAxis(m_State->Devices, phase, action);
		const float injected = m_State->Injected[action].Views[Utils::PhaseIndex(phase)].Value;
		return std::abs(injected) >= std::abs(bound) ? injected : bound;
	}

	PlayInputSummary PlayInput::GetSummary(InputPhase phase) const
	{
		const InputState& devices = m_State->Devices;
		PlayInputSummary summary;
		const auto add = [&summary](std::string name, bool down, bool pressed, bool released)
		{
			if (down)
				summary.Down.push_back(name);
			if (pressed)
				summary.Pressed.push_back(name);
			if (released)
				summary.Released.push_back(std::move(name));
		};

		for (size_t code = 0; code < KeyCodeCount; ++code)
		{
			const auto key = static_cast<Key>(code);
			if (KeyToString(key).empty())
				continue;
			const bool down = devices.IsKeyDown(phase, key);
			const bool pressed = devices.WasKeyPressed(phase, key);
			const bool released = devices.WasKeyReleased(phase, key);
			if (down || pressed || released)
				add(Utils::MakeControlName(key), down, pressed, released);
		}
		for (size_t code = 0; code < MouseButtonCount; ++code)
		{
			const auto button = static_cast<MouseButton>(code);
			const bool down = devices.IsMouseButtonDown(phase, button);
			const bool pressed = devices.WasMouseButtonPressed(phase, button);
			const bool released = devices.WasMouseButtonReleased(phase, button);
			if (down || pressed || released)
				add(Utils::MakeControlName(button), down, pressed, released);
		}
		for (size_t code = 0; code < GamepadButtonCount; ++code)
		{
			const auto button = static_cast<GamepadButton>(code);
			bool down = false;
			bool pressed = false;
			bool released = false;
			for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
			{
				if (!devices.IsGamepadConnected(phase, gamepad))
					continue;
				down = down || devices.IsGamepadButtonDown(phase, gamepad, button);
				pressed = pressed || devices.WasGamepadButtonPressed(phase, gamepad, button);
				released = released || devices.WasGamepadButtonReleased(phase, gamepad, button);
			}
			if (down || pressed || released)
				add(Utils::MakeControlName(button), down, pressed, released);
		}
		for (size_t code = 0; code < GamepadAxisCount; ++code)
		{
			const auto axis = static_cast<GamepadAxis>(code);
			const float value = Utils::ReadLargestGamepadAxis(devices, phase, axis);
			if (value != 0.0f)
				summary.Axes.emplace(Utils::MakeControlName(axis), value);
		}

		const InputActionMap& actions = m_State->Actions;
		for (uint32_t action = 0; action < actions.GetActionCount(); ++action)
		{
			const InputActionDefinition& definition = actions.GetDefinition(action);
			std::string name = std::format("Action.{}", definition.Name);
			const bool down = IsActionDown(phase, action);
			const bool pressed = WasActionPressed(phase, action);
			const bool released = WasActionReleased(phase, action);
			if (definition.Type == InputActionType::Axis)
			{
				const float value = GetActionAxis(phase, action);
				if (value != 0.0f)
					summary.Axes.emplace(name, value);
			}
			if (down || pressed || released)
				add(std::move(name), down, pressed, released);
		}

		for (std::vector<std::string>* list : { &summary.Down, &summary.Pressed, &summary.Released })
			std::sort(list->begin(), list->end());
		return summary;
	}

	std::string_view PlayInputEventTypeToString(PlayInputEventType type)
	{
		switch (type)
		{
			case PlayInputEventType::Action:
				return "Action";
			case PlayInputEventType::KeyInput:
				return "Key";
			case PlayInputEventType::MouseButtonInput:
				return "MouseButton";
			case PlayInputEventType::MouseMove:
				return "MouseMove";
			case PlayInputEventType::MouseDelta:
				return "MouseDelta";
			case PlayInputEventType::Scroll:
				return "Scroll";
			case PlayInputEventType::GamepadButtonInput:
				return "GamepadButton";
			case PlayInputEventType::GamepadAxisInput:
				return "GamepadAxis";
			case PlayInputEventType::Text:
				return "Text";
		}
		ENGINE_CORE_ASSERT(false, "unknown PlayInputEventType");
		return "Unknown";
	}

	std::string_view PlayInputEventStateToString(PlayInputEventState state)
	{
		switch (state)
		{
			case PlayInputEventState::Down:
				return "Down";
			case PlayInputEventState::Up:
				return "Up";
			case PlayInputEventState::Tap:
				return "Tap";
		}
		ENGINE_CORE_ASSERT(false, "unknown PlayInputEventState");
		return "Unknown";
	}

}
