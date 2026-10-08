#include "EnginePCH.h"
#include "Engine/Automation/Methods/InputMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/Private/PlayMethodSupport.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The members an InputEvent may carry (camelCase keys), in the order errors report them.
		constexpr std::array<std::string_view, 12> EventMembers = { "tick", "type", "name", "state", "value", "key", "button", "gamepad", "axis",
			"position", "delta", "text" };

		// Which members an event type reads, and which of them it cannot do without (§13.6).
		struct EventShape
		{
			std::span<const std::string_view> Allowed{};
			std::span<const std::string_view> Required{};
		};

		constexpr std::array<std::string_view, 3> ActionMembers = { "name", "state", "value" };
		constexpr std::array<std::string_view, 2> KeyMembers = { "key", "state" };
		constexpr std::array<std::string_view, 3> MouseButtonMembers = { "button", "state", "position" };
		constexpr std::array<std::string_view, 1> PositionMembers = { "position" };
		constexpr std::array<std::string_view, 1> DeltaMembers = { "delta" };
		constexpr std::array<std::string_view, 3> GamepadButtonMembers = { "gamepad", "button", "state" };
		constexpr std::array<std::string_view, 3> GamepadAxisMembers = { "gamepad", "axis", "value" };
		constexpr std::array<std::string_view, 1> TextMembers = { "text" };

		constexpr std::array<std::string_view, 1> RequiredName = { "name" };
		constexpr std::array<std::string_view, 1> RequiredKey = { "key" };
		constexpr std::array<std::string_view, 1> RequiredButton = { "button" };
		constexpr std::array<std::string_view, 2> RequiredAxisValue = { "axis", "value" };

	}

	namespace Utils {

		static EventShape GetEventShape(PlayInputEventType type)
		{
			switch (type)
			{
				case PlayInputEventType::Action:             return { ActionMembers, RequiredName };
				case PlayInputEventType::KeyInput:           return { KeyMembers, RequiredKey };
				case PlayInputEventType::MouseButtonInput:   return { MouseButtonMembers, RequiredButton };
				case PlayInputEventType::MouseMove:          return { PositionMembers, PositionMembers };
				case PlayInputEventType::MouseDelta:         return { DeltaMembers, DeltaMembers };
				case PlayInputEventType::Scroll:             return { DeltaMembers, DeltaMembers };
				case PlayInputEventType::GamepadButtonInput: return { GamepadButtonMembers, RequiredButton };
				case PlayInputEventType::GamepadAxisInput:   return { GamepadAxisMembers, RequiredAxisValue };
				case PlayInputEventType::Text:               return { TextMembers, TextMembers };
			}
			return {};
		}

		// The value at `pointer` ("/events/2") of the request's params: an object member per name segment, an array element
		// per index segment. nullopt when nothing is there.
		static std::optional<JsonReader> FindParamValue(const Json& params, std::string_view pointer)
		{
			JsonReader current(params);
			size_t start = 1;
			while (start <= pointer.size() && !pointer.empty())
			{
				const size_t end = std::min(pointer.find('/', start), pointer.size());
				const std::string_view segment = pointer.substr(start, end - start);
				if (current.IsArray())
				{
					size_t index = 0;
					const std::from_chars_result parsed = std::from_chars(segment.data(), segment.data() + segment.size(), index);
					if (parsed.ec != std::errc() || parsed.ptr != segment.data() + segment.size())
						return std::nullopt;
					Result<JsonReader> element = current.GetElement(index);
					if (!element)
						return std::nullopt;
					current = std::move(*element);
				}
				else
				{
					std::optional<JsonReader> member = current.FindMember(segment);
					if (!member.has_value())
						return std::nullopt;
					current = std::move(*member);
				}
				start = end + 1;
			}
			return current;
		}

		// Every name `toString` gives for the codes 0 .. count - 1, for "did you mean" hints.
		template<typename Code, typename ToString>
		static std::vector<std::string> CollectNames(size_t count, ToString toString)
		{
			std::vector<std::string> names;
			for (size_t code = 0; code < count; ++code)
			{
				const std::string_view name = toString(static_cast<Code>(code));
				if (!name.empty())
					names.emplace_back(name);
			}
			return names;
		}

		// InvalidArgument at `pointer` for an unknown control name, with "did you mean" suggestions from `names`.
		static Error MakeUnknownNameError(std::string_view pointer, std::string_view kind, std::string_view name, std::span<const std::string> names,
			std::string_view example)
		{
			const std::vector<std::string> suggestions = FuzzySuggest(name, names);
			std::string hint = MakeDidYouMeanHint(suggestions);
			if (hint.empty())
				hint = std::format("name a {} such as {}", kind, example);
			return MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("unknown {} '{}'", kind, name), std::move(hint));
		}

	}

	namespace Automation {

		Result<PlayInputEvent> MakePlayInputEvent(const AutomationMethodContext& context, const InputEventParams& event, std::string_view pointer,
			const InputActionMap& actions)
		{
			// Presence is read from the request itself: a nested struct's members have no HasParam (InputMethods.h).
			const std::optional<JsonReader> object = Utils::FindParamValue(context.GetParams(), pointer);
			const auto has = [&object](std::string_view member)
			{
				return object.has_value() && object->HasMember(member);
			};
			if (!has("type"))
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/type", pointer), "an input event needs a type",
					"give type: action, key, mouseButton, mouseMove, mouseDelta, scroll, gamepadButton, gamepadAxis or text"));

			const EventShape shape = Utils::GetEventShape(event.Type);
			const std::string_view typeName = PlayInputEventTypeToString(event.Type);
			for (const std::string_view member : EventMembers)
			{
				if (member == "tick" || member == "type" || !has(member))
					continue;
				if (std::find(shape.Allowed.begin(), shape.Allowed.end(), member) == shape.Allowed.end())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, member),
						std::format("a {} event has no member '{}'", typeName, member), "remove it; each event type reads only its own members (§13.6)"));
				}
			}
			for (const std::string_view member : shape.Required)
			{
				if (!has(member))
				{
					return std::unexpected(
						Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/{}", pointer, member), std::format("a {} event needs '{}'", typeName, member)));
				}
			}
			if (event.Type == PlayInputEventType::Action && has("state") && has("value"))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("{}/value", pointer),
					"an action event sets a state (a button use) or a value (an axis value), not both", "remove state or value"));
			}

			PlayInputEvent converted;
			converted.Type = event.Type;
			converted.State = event.State;
			switch (event.Type)
			{
				case PlayInputEventType::Action:
					converted.Name = event.Name;
					converted.HasValue = has("value");
					converted.Value = event.Value;
					break;
				case PlayInputEventType::KeyInput:
				{
					const std::optional<Key> key = KeyFromString(event.KeyName);
					if (!key.has_value())
					{
						const std::vector<std::string> names = Utils::CollectNames<Key>(KeyCodeCount, &KeyToString);
						return std::unexpected(Utils::MakeUnknownNameError(std::format("{}/key", pointer), "key", event.KeyName, names, "\"Space\" or \"A\""));
					}
					converted.KeyCode = *key;
					break;
				}
				case PlayInputEventType::MouseButtonInput:
				{
					const std::optional<MouseButton> button = MouseButtonFromString(event.ButtonName);
					if (!button.has_value())
					{
						const std::vector<std::string> names = Utils::CollectNames<MouseButton>(MouseButtonCount, &MouseButtonToString);
						return std::unexpected(
							Utils::MakeUnknownNameError(std::format("{}/button", pointer), "mouse button", event.ButtonName, names, "\"Left\" or \"Right\""));
					}
					converted.Button = *button;
					converted.HasPosition = has("position");
					converted.Position = event.Position;
					break;
				}
				case PlayInputEventType::MouseMove:
					converted.Position = event.Position;
					break;
				case PlayInputEventType::MouseDelta:
				case PlayInputEventType::Scroll:
					converted.Delta = event.Delta;
					break;
				case PlayInputEventType::GamepadButtonInput:
				{
					const std::optional<GamepadButton> button = GamepadButtonFromString(event.ButtonName);
					if (!button.has_value())
					{
						const std::vector<std::string> names = Utils::CollectNames<GamepadButton>(GamepadButtonCount, &GamepadButtonToString);
						return std::unexpected(
							Utils::MakeUnknownNameError(std::format("{}/button", pointer), "gamepad button", event.ButtonName, names, "\"South\" or \"Start\""));
					}
					converted.Gamepad = event.Gamepad;
					converted.GamepadButtonCode = *button;
					break;
				}
				case PlayInputEventType::GamepadAxisInput:
				{
					const std::optional<GamepadAxis> axis = GamepadAxisFromString(event.AxisName);
					if (!axis.has_value())
					{
						const std::vector<std::string> names = Utils::CollectNames<GamepadAxis>(GamepadAxisCount, &GamepadAxisToString);
						return std::unexpected(
							Utils::MakeUnknownNameError(std::format("{}/axis", pointer), "gamepad axis", event.AxisName, names, "\"LeftX\" or \"RightTrigger\""));
					}
					converted.Gamepad = event.Gamepad;
					converted.Axis = *axis;
					converted.Value = event.Value;
					break;
				}
				case PlayInputEventType::Text:
					converted.Text = event.Text;
					break;
			}

			const Status valid = ValidatePlayInputEvent(converted, actions);
			if (!valid)
				return std::unexpected(Utils::PrefixPointers(valid.error(), pointer));
			return converted;
		}

		Result<InputInjectResult> InputInject(AutomationMethodContext& context, const InputInjectParams& params)
		{
			ENGINE_TRY_ASSIGN(PlaySession * session, Utils::RequirePlaySession(context));
			ENGINE_TRY(Utils::CheckLockstepOwner(context, *session));

			// All or nothing: every event is checked before any is queued.
			PlayInput& input = session->GetInput();
			std::vector<PlayInputEvent> events;
			events.reserve(params.Events.size());
			for (size_t index = 0; index < params.Events.size(); ++index)
			{
				ENGINE_TRY_ASSIGN(PlayInputEvent event, MakePlayInputEvent(context, params.Events[index], std::format("/events/{}", index), input.GetActions()));
				events.push_back(std::move(event));
			}

			const uint64_t first = session->GetTick();
			if (params.ReleaseAll)
				input.QueueReleaseAll(first);
			for (size_t index = 0; index < events.size(); ++index)
				ENGINE_TRY(input.Queue(first + params.Events[index].Tick, events[index]));
			return InputInjectResult{ .Tick = ToAutomationCounter(first), .Queued = ToAutomationCounter(events.size()) };
		}

	}

	void RegisterInputMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<PlayInputEventType>("InputEventType", "The type of an input event (§13.6).")
			.Entry(PlayInputEventType::Action, "Action", "A project action: a button use (state) or an axis value (value).")
			.Entry(PlayInputEventType::KeyInput, "Key", "A keyboard key going down or up.")
			.Entry(PlayInputEventType::MouseButtonInput, "MouseButton", "A mouse button going down or up, optionally at a position.")
			.Entry(PlayInputEventType::MouseMove, "MouseMove", "The cursor moving to a position (window coordinates).")
			.Entry(PlayInputEventType::MouseDelta, "MouseDelta", "The cursor moving by a delta.")
			.Entry(PlayInputEventType::Scroll, "Scroll", "Scrolling by a delta (x right-positive, y up-positive).")
			.Entry(PlayInputEventType::GamepadButtonInput, "GamepadButton", "A gamepad button going down or up.")
			.Entry(PlayInputEventType::GamepadAxisInput, "GamepadAxis", "A gamepad axis taking a value.")
			.Entry(PlayInputEventType::Text, "Text", "Typed text, one character event per code point.");

		registry.Enum<PlayInputEventState>("InputEventState", "The state of a button-like input event (§13.6).")
			.Entry(PlayInputEventState::Down, "Down", "Pressed on its tick.")
			.Entry(PlayInputEventState::Up, "Up", "Released on its tick.")
			.Entry(PlayInputEventState::Tap, "Tap", "Pressed on its tick and released on the next, so both edges are observed.");

		const FieldMeta unitMeta{ .Min = -1.0, .Max = 1.0 };
		registry.Struct<InputEventParams>("InputEvent", "One input event, stamped with a tick offset (§13.6); each type reads only its own members.")
			.Field("tick", &InputEventParams::Tick,
				"The tick offset: from the session's next tick (input.inject) or from the first tick of the play.step that carries it.")
			.Field("type", &InputEventParams::Type, "The event type (required).")
			.Field("name", &InputEventParams::Name, "action: the project action's name (exact, case-sensitive).")
			.Field("state", &InputEventParams::State, "action, key, mouseButton, gamepadButton: down (the default), up or tap.")
			.Field("value", &InputEventParams::Value, "action: an axis value in [-1, 1]; gamepadAxis: [-1, 1] for sticks (up positive), [0, 1] for triggers.",
				unitMeta)
			.Field("key", &InputEventParams::KeyName, "key: the key's name, such as \"Space\", \"A\" or \"Escape\".")
			.Field("button", &InputEventParams::ButtonName, "mouseButton: \"Left\", \"Right\", \"Middle\" ...; gamepadButton: \"South\", \"Start\" ...")
			.Field("gamepad", &InputEventParams::Gamepad, "gamepadButton, gamepadAxis: the gamepad, 0 to 3.",
				{ .Min = 0.0, .Max = static_cast<double>(MaxGamepads - 1) })
			.Field("axis", &InputEventParams::AxisName, "gamepadAxis: \"LeftX\", \"LeftY\", \"RightX\", \"RightY\", \"LeftTrigger\" or \"RightTrigger\".")
			.Field("position", &InputEventParams::Position, "mouseMove (required), mouseButton (optional): [x, y] in window coordinates.")
			.Field("delta", &InputEventParams::Delta, "mouseDelta: the cursor motion [x, y]; scroll: [x, y], y up-positive.")
			.Field("text", &InputEventParams::Text, "text: the typed text (UTF-8).");

		registry.Struct<PlayInputSummary>("PlayInputSummary", "The game input as one phase's view saw it, by binding name, each list sorted.")
			.Field("down", &PlayInputSummary::Down, "The controls and actions held (\"Key.Space\", \"Action.Jump\").")
			.Field("pressed", &PlayInputSummary::Pressed, "The controls and actions that went down since the view before.")
			.Field("released", &PlayInputSummary::Released, "The controls and actions that went up since the view before.")
			.Field("axes", &PlayInputSummary::Axes, "The non-zero gamepad axes (\"Gamepad.LeftX\") and Axis actions (\"Action.MoveX\").");

		registry.Struct<InputInjectParams>("InputInjectParams", "The params of input.inject: events for the play session's next ticks.")
			.Field("events", &InputInjectParams::Events, "The events, each with a tick offset from the session's next tick (required; may be empty).")
			.Field("releaseAll", &InputInjectParams::ReleaseAll,
				"First release every held key, button and injected action, and zero every axis, at the next tick.");

		registry.Struct<InputInjectResult>("InputInjectResult", "What input.inject queued.")
			.Field("tick", &InputInjectResult::Tick, "The tick the offsets count from: the session's next tick.")
			.Field("queued", &InputInjectResult::Queued, "The events queued (a tap counts once).");
	}

	void RegisterInputMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		Json tap = Json::object();
		tap["tick"] = 0;
		tap["type"] = "action";
		tap["name"] = "Jump";
		tap["state"] = "tap";
		example["events"] = Json::array({ tap });
		methods.Add(
			{
				.Name = "input.inject",
				.Description = "Queues input events for the play session: each is applied at step 1 of its tick (an offset from the session's "
							   "next tick), like real input and replays. Every event is checked before any is queued.",
				.RequiredParams = { "events" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.Examples = { { .Description = "Tap the Jump action on the next tick.", .Params = example } },
			},
			&Automation::InputInject);
	}

}
