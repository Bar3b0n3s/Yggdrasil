#include "EnginePCH.h"
#include "Engine/Platform/Input/InputActionMap.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <set>

namespace Engine {

	namespace {

		enum class ButtonQuery : uint8_t
		{
			Down,
			Pressed,
			Released
		};

		// The device prefix and the control name of a binding; the name is empty for a value outside its enumeration.
		struct ControlName
		{
			std::string_view Device{};
			std::string_view Name{};
		};

	}

	namespace Utils {

		static char ToAsciiLower(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Device prefixes are matched like automation enum values (§13.4): only 'A'-'Z' fold.
		static bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs)
		{
			return std::ranges::equal(lhs, rhs, [](char left, char right)
			{
				return ToAsciiLower(left) == ToAsciiLower(right);
			});
		}

		static std::unexpected<Error> MakeBindingError(std::string message, std::string hint)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::move(message)).WithHint(std::move(hint)));
		}

		static bool QueryGamepadButtonOf(const InputState& input, InputPhase phase, uint32_t gamepad, GamepadButton button,
			ButtonQuery query)
		{
			switch (query)
			{
				case ButtonQuery::Down:     return input.IsGamepadButtonDown(phase, gamepad, button);
				case ButtonQuery::Pressed:  return input.WasGamepadButtonPressed(phase, gamepad, button);
				case ButtonQuery::Released: return input.WasGamepadButtonReleased(phase, gamepad, button);
			}

			ENGINE_CORE_ASSERT(false, "Unknown ButtonQuery {}", std::to_underlying(query));
			return false;
		}

		static bool QueryGamepadButton(const InputState& input, InputPhase phase, GamepadButton button, ButtonQuery query)
		{
			// Every gamepad counts (v1 has one player). A gamepad that is not connected has no button down, and the
			// Released edges of a gamepad that disconnected while a button was held still count.
			for (uint32_t gamepad = 0; gamepad < MaxGamepads; ++gamepad)
			{
				if (QueryGamepadButtonOf(input, phase, gamepad, button, query))
					return true;
			}
			return false;
		}

		static bool QueryKey(const InputState& input, InputPhase phase, Key key, ButtonQuery query)
		{
			switch (query)
			{
				case ButtonQuery::Down:     return input.IsKeyDown(phase, key);
				case ButtonQuery::Pressed:  return input.WasKeyPressed(phase, key);
				case ButtonQuery::Released: return input.WasKeyReleased(phase, key);
			}

			ENGINE_CORE_ASSERT(false, "Unknown ButtonQuery {}", std::to_underlying(query));
			return false;
		}

		static bool QueryMouseButton(const InputState& input, InputPhase phase, MouseButton button, ButtonQuery query)
		{
			switch (query)
			{
				case ButtonQuery::Down:     return input.IsMouseButtonDown(phase, button);
				case ButtonQuery::Pressed:  return input.WasMouseButtonPressed(phase, button);
				case ButtonQuery::Released: return input.WasMouseButtonReleased(phase, button);
			}

			ENGINE_CORE_ASSERT(false, "Unknown ButtonQuery {}", std::to_underlying(query));
			return false;
		}

		static bool QueryControl(const InputState& input, InputPhase phase, const InputBinding::Control& control, ButtonQuery query)
		{
			if (const Key* key = std::get_if<Key>(&control))
				return QueryKey(input, phase, *key, query);
			if (const MouseButton* mouseButton = std::get_if<MouseButton>(&control))
				return QueryMouseButton(input, phase, *mouseButton, query);
			if (const GamepadButton* gamepadButton = std::get_if<GamepadButton>(&control))
				return QueryGamepadButton(input, phase, *gamepadButton, query);
			// Create keeps gamepad axes out of the button lists.
			return false;
		}

		// True when any of `controls` satisfies `query` in the view of `phase`.
		static bool QueryAny(const InputState& input, InputPhase phase, std::span<const InputBinding::Control> controls,
			ButtonQuery query)
		{
			return std::ranges::any_of(controls, [&input, phase, query](const InputBinding::Control& control)
			{
				return QueryControl(input, phase, control, query);
			});
		}

		static ControlName GetControlName(const InputBinding::Control& control)
		{
			if (const Key* key = std::get_if<Key>(&control))
				return { "Key", KeyToString(*key) };
			if (const MouseButton* mouseButton = std::get_if<MouseButton>(&control))
				return { "Mouse", MouseButtonToString(*mouseButton) };
			if (const GamepadButton* gamepadButton = std::get_if<GamepadButton>(&control))
				return { "Gamepad", GamepadButtonToString(*gamepadButton) };
			if (const GamepadAxis* gamepadAxis = std::get_if<GamepadAxis>(&control))
				return { "Gamepad", GamepadAxisToString(*gamepadAxis) };
			return {};
		}

		static ErrorIssue MakeIssue(std::string pointer, std::string message, std::string hint = {})
		{
			return ErrorIssue{
				.JsonPointer = std::move(pointer),
				.Message = std::move(message),
				.Hint = std::move(hint),
				.Suggestions = {},
			};
		}

		// The pointer "/<action>/<field>" of an issue.
		static std::string MakeIssuePointer(std::string_view action, std::string_view field)
		{
			return JsonReader::AppendPointer(JsonReader::AppendPointer("", action), field);
		}

		// Parses the button bindings `names`, the field `field` of `action`, into `controls`, with an issue for every
		// unknown name and every gamepad axis.
		static void ResolveButtonBindings(std::string_view action, std::string_view field, std::span<const std::string> names,
			std::vector<InputBinding::Control>& controls, std::vector<ErrorIssue>& issues)
		{
			const std::string fieldPointer = MakeIssuePointer(action, field);
			for (size_t index = 0; index < names.size(); ++index)
			{
				const Result<InputBinding> binding = InputBinding::Parse(names[index]);
				if (!binding.has_value())
				{
					issues.push_back(MakeIssue(JsonReader::AppendPointer(fieldPointer, index), binding.error().GetMessageText(),
						binding.error().GetHint()));
				}
				else if (std::holds_alternative<GamepadAxis>(binding->Target))
				{
					issues.push_back(MakeIssue(JsonReader::AppendPointer(fieldPointer, index),
						std::format("'{}' is a gamepad axis, not a button", names[index]),
						"bind keys, mouse buttons and gamepad buttons here; a gamepad axis belongs in an Axis action's Gamepad"));
				}
				else
				{
					controls.push_back(binding->Target);
				}
			}
		}

		// The gamepad axis of an Axis action (`name`, not empty), or nullopt with an issue.
		static std::optional<GamepadAxis> ResolveGamepadAxis(std::string_view action, const std::string& name,
			std::vector<ErrorIssue>& issues)
		{
			const Result<InputBinding> binding = InputBinding::Parse(name);
			if (!binding.has_value())
			{
				issues.push_back(MakeIssue(MakeIssuePointer(action, "Gamepad"), binding.error().GetMessageText(),
					binding.error().GetHint()));
				return std::nullopt;
			}

			if (const GamepadAxis* axis = std::get_if<GamepadAxis>(&binding->Target))
				return *axis;

			issues.push_back(MakeIssue(MakeIssuePointer(action, "Gamepad"), std::format("'{}' is not a gamepad axis", name),
				"write one of Gamepad.LeftX, Gamepad.LeftY, Gamepad.RightX, Gamepad.RightY, Gamepad.LeftTrigger and "
				"Gamepad.RightTrigger, or leave it empty"));
			return std::nullopt;
		}

		// An issue for a field that `type` actions do not have.
		static ErrorIssue MakeUnexpectedFieldIssue(std::string_view action, std::string_view field, std::string_view type,
			std::string_view hint)
		{
			return MakeIssue(MakeIssuePointer(action, field), std::format("a {} action has no {}", type, field),
				std::string(hint));
		}

	}

	Result<InputBinding> InputBinding::Parse(std::string_view name)
	{
		const size_t separator = name.find('.');
		if (separator == std::string_view::npos)
		{
			return Utils::MakeBindingError(std::format("input binding '{}' has no device prefix", name),
				"write the device and the control, such as 'Key.Space', 'Mouse.Left', 'Gamepad.South' or 'Gamepad.LeftX'");
		}

		const std::string_view device = name.substr(0, separator);
		const std::string_view control = name.substr(separator + 1);
		if (Utils::EqualsIgnoreAsciiCase(device, "Key"))
		{
			if (const std::optional<Key> key = KeyFromString(control))
				return InputBinding{ .Target = *key };
			return Utils::MakeBindingError(std::format("unknown key '{}' in input binding '{}'", control, name),
				"keys are named after their US-layout position, such as 'Key.Space', 'Key.A', 'Key.D0', 'Key.Keypad0' or "
				"'Key.LeftShift'");
		}
		if (Utils::EqualsIgnoreAsciiCase(device, "Mouse"))
		{
			if (const std::optional<MouseButton> button = MouseButtonFromString(control))
				return InputBinding{ .Target = *button };
			return Utils::MakeBindingError(std::format("unknown mouse button '{}' in input binding '{}'", control, name),
				"mouse buttons are Left, Right, Middle and Button4 to Button8");
		}
		if (Utils::EqualsIgnoreAsciiCase(device, "Gamepad"))
		{
			if (const std::optional<GamepadButton> button = GamepadButtonFromString(control))
				return InputBinding{ .Target = *button };
			if (const std::optional<GamepadAxis> axis = GamepadAxisFromString(control))
				return InputBinding{ .Target = *axis };
			return Utils::MakeBindingError(std::format("unknown gamepad control '{}' in input binding '{}'", control, name),
				"gamepad controls are buttons by position (South, East, West, North, DPadUp, Start ...) and axes (LeftX, "
				"LeftY, RightX, RightY, LeftTrigger, RightTrigger)");
		}

		return Utils::MakeBindingError(std::format("unknown device '{}' in input binding '{}'", device, name),
			"the devices are Key, Mouse and Gamepad");
	}

	std::string InputBinding::ToString() const
	{
		const ControlName control = Utils::GetControlName(Target);
		if (control.Name.empty())
			return {};
		return std::format("{}.{}", control.Device, control.Name);
	}

	Result<InputActionMap> InputActionMap::Create(std::span<const InputActionDefinition> definitions)
	{
		std::vector<ErrorIssue> issues;
		std::vector<ResolvedAction> resolvedActions(definitions.size());
		std::set<std::string_view> seenNames;
		for (size_t index = 0; index < definitions.size(); ++index)
		{
			const InputActionDefinition& definition = definitions[index];
			ResolvedAction& resolved = resolvedActions[index];
			const std::string_view name = definition.Name;

			if (name.empty())
			{
				issues.push_back(Utils::MakeIssue(Utils::MakeIssuePointer(name, "Name"), "the action name is empty"));
			}
			else if (!seenNames.insert(name).second)
			{
				issues.push_back(Utils::MakeIssue(Utils::MakeIssuePointer(name, "Name"),
					std::format("the action name '{}' is used more than once", name)));
			}

			bool isKnownType = false;
			switch (definition.Type)
			{
				case InputActionType::Button:
				{
					isKnownType = true;
					Utils::ResolveButtonBindings(name, "Bindings", definition.Bindings, resolved.Bindings, issues);
					constexpr std::string_view UseBindings = "a Button action lists its keys and buttons in Bindings";
					if (!definition.Positive.empty())
						issues.push_back(Utils::MakeUnexpectedFieldIssue(name, "Positive", "Button", UseBindings));
					if (!definition.Negative.empty())
						issues.push_back(Utils::MakeUnexpectedFieldIssue(name, "Negative", "Button", UseBindings));
					if (!definition.Gamepad.empty())
						issues.push_back(Utils::MakeUnexpectedFieldIssue(name, "Gamepad", "Button", "make it an Axis action"));
					if (definition.Invert)
						issues.push_back(Utils::MakeUnexpectedFieldIssue(name, "Invert", "Button", "Invert flips an Axis action's Gamepad"));
					break;
				}
				case InputActionType::Axis:
				{
					isKnownType = true;
					if (!definition.Bindings.empty())
					{
						issues.push_back(Utils::MakeUnexpectedFieldIssue(name, "Bindings", "Axis",
							"an Axis action lists its keys and buttons in Positive and Negative"));
					}
					Utils::ResolveButtonBindings(name, "Positive", definition.Positive, resolved.Positive, issues);
					Utils::ResolveButtonBindings(name, "Negative", definition.Negative, resolved.Negative, issues);
					if (!definition.Gamepad.empty())
						resolved.Gamepad = Utils::ResolveGamepadAxis(name, definition.Gamepad, issues);
					break;
				}
			}
			if (!isKnownType)
			{
				issues.push_back(Utils::MakeIssue(Utils::MakeIssuePointer(name, "Type"),
					std::format("unknown action type {}", std::to_underlying(definition.Type)), "the action types are Button and Axis"));
			}
		}

		if (!issues.empty())
		{
			std::string message = std::format("{} problem{} in the input action definitions", issues.size(),
				issues.size() == 1 ? "" : "s");
			return std::unexpected(Error(ErrorCode::Validation, std::move(message)).WithIssues(std::move(issues)));
		}

		// Index in byte-wise name order (std::string compares bytes as unsigned char), whatever the definition order.
		std::vector<size_t> order(definitions.size());
		std::iota(order.begin(), order.end(), size_t{ 0 });
		std::ranges::sort(order, [definitions](size_t lhs, size_t rhs)
		{
			return definitions[lhs].Name < definitions[rhs].Name;
		});

		InputActionMap map;
		map.m_Definitions.reserve(definitions.size());
		map.m_Actions.reserve(definitions.size());
		for (const size_t index : order)
		{
			map.m_Definitions.push_back(definitions[index]);
			map.m_Actions.push_back(std::move(resolvedActions[index]));
		}
		return map;
	}

	std::optional<uint32_t> InputActionMap::FindAction(std::string_view name) const
	{
		const auto found = std::ranges::lower_bound(m_Definitions, name, std::ranges::less{}, [](const InputActionDefinition& definition)
		{
			return std::string_view(definition.Name);
		});
		if (found == m_Definitions.end() || found->Name != name)
			return std::nullopt;
		return static_cast<uint32_t>(found - m_Definitions.begin());
	}

	uint32_t InputActionMap::GetActionCount() const
	{
		return static_cast<uint32_t>(m_Definitions.size());
	}

	const InputActionDefinition& InputActionMap::GetDefinition(uint32_t action) const
	{
		if (IsValidAction(action))
			return m_Definitions[action];

		static const InputActionDefinition EmptyDefinition;
		return EmptyDefinition;
	}

	bool InputActionMap::IsDown(const InputState& input, InputPhase phase, uint32_t action) const
	{
		if (!IsValidAction(action))
			return false;

		const ResolvedAction& resolved = m_Actions[action];
		return Utils::QueryAny(input, phase, resolved.Bindings, ButtonQuery::Down)
			|| Utils::QueryAny(input, phase, resolved.Positive, ButtonQuery::Down)
			|| Utils::QueryAny(input, phase, resolved.Negative, ButtonQuery::Down);
	}

	bool InputActionMap::WasPressed(const InputState& input, InputPhase phase, uint32_t action) const
	{
		if (!IsValidAction(action))
			return false;

		const ResolvedAction& resolved = m_Actions[action];
		return Utils::QueryAny(input, phase, resolved.Bindings, ButtonQuery::Pressed)
			|| Utils::QueryAny(input, phase, resolved.Positive, ButtonQuery::Pressed)
			|| Utils::QueryAny(input, phase, resolved.Negative, ButtonQuery::Pressed);
	}

	bool InputActionMap::WasReleased(const InputState& input, InputPhase phase, uint32_t action) const
	{
		if (!IsValidAction(action))
			return false;

		const ResolvedAction& resolved = m_Actions[action];
		return Utils::QueryAny(input, phase, resolved.Bindings, ButtonQuery::Released)
			|| Utils::QueryAny(input, phase, resolved.Positive, ButtonQuery::Released)
			|| Utils::QueryAny(input, phase, resolved.Negative, ButtonQuery::Released);
	}

	float InputActionMap::GetAxis(const InputState& input, InputPhase phase, uint32_t action) const
	{
		if (!IsValidAction(action))
			return 0.0f;

		if (m_Definitions[action].Type == InputActionType::Button)
			return IsDown(input, phase, action) ? 1.0f : 0.0f;

		const ResolvedAction& resolved = m_Actions[action];
		const float positive = Utils::QueryAny(input, phase, resolved.Positive, ButtonQuery::Down) ? 1.0f : 0.0f;
		const float negative = Utils::QueryAny(input, phase, resolved.Negative, ButtonQuery::Down) ? 1.0f : 0.0f;
		const float keys = positive - negative;

		float gamepad = 0.0f;
		if (resolved.Gamepad.has_value())
		{
			const bool invert = m_Definitions[action].Invert;
			for (uint32_t index = 0; index < MaxGamepads; ++index)
			{
				if (!input.IsGamepadConnected(phase, index))
					continue;
				const float raw = input.GetGamepadAxis(phase, index, *resolved.Gamepad);
				// 0 - raw rather than -raw, so that a centred axis stays +0.
				const float value = ApplyDeadZone(invert ? 0.0f - raw : raw);
				// The largest magnitude wins; on a tie the lower gamepad index keeps it.
				if (std::abs(value) > std::abs(gamepad))
					gamepad = value;
			}
		}

		// The larger magnitude wins; on a tie the key value.
		return std::abs(gamepad) > std::abs(keys) ? gamepad : keys;
	}

	float InputActionMap::ApplyDeadZone(float value)
	{
		const float clamped = std::clamp(value, -1.0f, 1.0f);
		const float magnitude = std::abs(clamped);
		// Written so that NaN, which compares false, also reads 0.
		if (!(magnitude > DeadZone))
			return 0.0f;

		// At full deflection the numerator and the denominator are the same float, so the result is exactly 1.
		const float rescaled = (magnitude - DeadZone) / (1.0f - DeadZone);
		return clamped < 0.0f ? -rescaled : rescaled;
	}

	bool InputActionMap::IsValidAction(uint32_t action) const
	{
		const bool valid = action < m_Definitions.size();
		ENGINE_CORE_ASSERT(valid, "Input action index {} is out of range ({} actions)", action, m_Definitions.size());
		return valid;
	}

}
