#include "EnginePCH.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <algorithm>
#include <limits>

namespace Engine {

	namespace Utils {

		static char ToAsciiLower(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// Names are matched like automation enum values (§13.4): only 'A'-'Z' fold, every other byte must match exactly.
		static bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs)
		{
			return std::ranges::equal(lhs, rhs, [](char left, char right)
			{
				return ToAsciiLower(left) == ToAsciiLower(right);
			});
		}

		// The enumerator name, or an empty view for Key::None and for a value that names no key. A switch rather than a
		// table, so that an enumerator added without a name is a -Wswitch error on GCC and Clang.
		static std::string_view TryGetKeyName(Key key)
		{
			switch (key)
			{
				case Key::None:           return {};
				case Key::Space:          return "Space";
				case Key::Apostrophe:     return "Apostrophe";
				case Key::Comma:          return "Comma";
				case Key::Minus:          return "Minus";
				case Key::Period:         return "Period";
				case Key::Slash:          return "Slash";
				case Key::D0:             return "D0";
				case Key::D1:             return "D1";
				case Key::D2:             return "D2";
				case Key::D3:             return "D3";
				case Key::D4:             return "D4";
				case Key::D5:             return "D5";
				case Key::D6:             return "D6";
				case Key::D7:             return "D7";
				case Key::D8:             return "D8";
				case Key::D9:             return "D9";
				case Key::Semicolon:      return "Semicolon";
				case Key::Equal:          return "Equal";
				case Key::A:              return "A";
				case Key::B:              return "B";
				case Key::C:              return "C";
				case Key::D:              return "D";
				case Key::E:              return "E";
				case Key::F:              return "F";
				case Key::G:              return "G";
				case Key::H:              return "H";
				case Key::I:              return "I";
				case Key::J:              return "J";
				case Key::K:              return "K";
				case Key::L:              return "L";
				case Key::M:              return "M";
				case Key::N:              return "N";
				case Key::O:              return "O";
				case Key::P:              return "P";
				case Key::Q:              return "Q";
				case Key::R:              return "R";
				case Key::S:              return "S";
				case Key::T:              return "T";
				case Key::U:              return "U";
				case Key::V:              return "V";
				case Key::W:              return "W";
				case Key::X:              return "X";
				case Key::Y:              return "Y";
				case Key::Z:              return "Z";
				case Key::LeftBracket:    return "LeftBracket";
				case Key::Backslash:      return "Backslash";
				case Key::RightBracket:   return "RightBracket";
				case Key::GraveAccent:    return "GraveAccent";
				case Key::World1:         return "World1";
				case Key::World2:         return "World2";
				case Key::Escape:         return "Escape";
				case Key::Enter:          return "Enter";
				case Key::Tab:            return "Tab";
				case Key::Backspace:      return "Backspace";
				case Key::Insert:         return "Insert";
				case Key::Delete:         return "Delete";
				case Key::Right:          return "Right";
				case Key::Left:           return "Left";
				case Key::Down:           return "Down";
				case Key::Up:             return "Up";
				case Key::PageUp:         return "PageUp";
				case Key::PageDown:       return "PageDown";
				case Key::Home:           return "Home";
				case Key::End:            return "End";
				case Key::CapsLock:       return "CapsLock";
				case Key::ScrollLock:     return "ScrollLock";
				case Key::NumLock:        return "NumLock";
				case Key::PrintScreen:    return "PrintScreen";
				case Key::Pause:          return "Pause";
				case Key::F1:             return "F1";
				case Key::F2:             return "F2";
				case Key::F3:             return "F3";
				case Key::F4:             return "F4";
				case Key::F5:             return "F5";
				case Key::F6:             return "F6";
				case Key::F7:             return "F7";
				case Key::F8:             return "F8";
				case Key::F9:             return "F9";
				case Key::F10:            return "F10";
				case Key::F11:            return "F11";
				case Key::F12:            return "F12";
				case Key::F13:            return "F13";
				case Key::F14:            return "F14";
				case Key::F15:            return "F15";
				case Key::F16:            return "F16";
				case Key::F17:            return "F17";
				case Key::F18:            return "F18";
				case Key::F19:            return "F19";
				case Key::F20:            return "F20";
				case Key::F21:            return "F21";
				case Key::F22:            return "F22";
				case Key::F23:            return "F23";
				case Key::F24:            return "F24";
				case Key::F25:            return "F25";
				case Key::Keypad0:        return "Keypad0";
				case Key::Keypad1:        return "Keypad1";
				case Key::Keypad2:        return "Keypad2";
				case Key::Keypad3:        return "Keypad3";
				case Key::Keypad4:        return "Keypad4";
				case Key::Keypad5:        return "Keypad5";
				case Key::Keypad6:        return "Keypad6";
				case Key::Keypad7:        return "Keypad7";
				case Key::Keypad8:        return "Keypad8";
				case Key::Keypad9:        return "Keypad9";
				case Key::KeypadDecimal:  return "KeypadDecimal";
				case Key::KeypadDivide:   return "KeypadDivide";
				case Key::KeypadMultiply: return "KeypadMultiply";
				case Key::KeypadSubtract: return "KeypadSubtract";
				case Key::KeypadAdd:      return "KeypadAdd";
				case Key::KeypadEnter:    return "KeypadEnter";
				case Key::KeypadEqual:    return "KeypadEqual";
				case Key::LeftShift:      return "LeftShift";
				case Key::LeftControl:    return "LeftControl";
				case Key::LeftAlt:        return "LeftAlt";
				case Key::LeftSuper:      return "LeftSuper";
				case Key::RightShift:     return "RightShift";
				case Key::RightControl:   return "RightControl";
				case Key::RightAlt:       return "RightAlt";
				case Key::RightSuper:     return "RightSuper";
				case Key::Menu:           return "Menu";
			}
			return {};
		}

		static std::string_view TryGetMouseButtonName(MouseButton button)
		{
			switch (button)
			{
				case MouseButton::Left:    return "Left";
				case MouseButton::Right:   return "Right";
				case MouseButton::Middle:  return "Middle";
				case MouseButton::Button4: return "Button4";
				case MouseButton::Button5: return "Button5";
				case MouseButton::Button6: return "Button6";
				case MouseButton::Button7: return "Button7";
				case MouseButton::Button8: return "Button8";
			}
			return {};
		}

		static std::string_view TryGetGamepadButtonName(GamepadButton button)
		{
			switch (button)
			{
				case GamepadButton::South:       return "South";
				case GamepadButton::East:        return "East";
				case GamepadButton::West:        return "West";
				case GamepadButton::North:       return "North";
				case GamepadButton::LeftBumper:  return "LeftBumper";
				case GamepadButton::RightBumper: return "RightBumper";
				case GamepadButton::Back:        return "Back";
				case GamepadButton::Start:       return "Start";
				case GamepadButton::Guide:       return "Guide";
				case GamepadButton::LeftThumb:   return "LeftThumb";
				case GamepadButton::RightThumb:  return "RightThumb";
				case GamepadButton::DPadUp:      return "DPadUp";
				case GamepadButton::DPadRight:   return "DPadRight";
				case GamepadButton::DPadDown:    return "DPadDown";
				case GamepadButton::DPadLeft:    return "DPadLeft";
			}
			return {};
		}

		static std::string_view TryGetGamepadAxisName(GamepadAxis axis)
		{
			switch (axis)
			{
				case GamepadAxis::LeftX:        return "LeftX";
				case GamepadAxis::LeftY:        return "LeftY";
				case GamepadAxis::RightX:       return "RightX";
				case GamepadAxis::RightY:       return "RightY";
				case GamepadAxis::LeftTrigger:  return "LeftTrigger";
				case GamepadAxis::RightTrigger: return "RightTrigger";
			}
			return {};
		}

		// The value of the first enumerator in [0, valueCount) whose name matches `name`; nullopt when none does. An empty
		// name never matches, because unnamed values (Key::None, gaps) have empty names.
		template<typename Code, typename NameFunction>
		static std::optional<Code> FindCodeByName(std::string_view name, size_t valueCount, NameFunction nameOf)
		{
			if (name.empty())
				return std::nullopt;

			for (size_t value = 0; value < valueCount; ++value)
			{
				const Code code = static_cast<Code>(value);
				if (EqualsIgnoreAsciiCase(nameOf(code), name))
					return code;
			}
			return std::nullopt;
		}

	}

	static_assert(KeyCodeCount <= std::numeric_limits<std::underlying_type_t<Key>>::max());
	static_assert(MouseButtonCount == static_cast<size_t>(MouseButton::Button8) + 1);
	static_assert(GamepadButtonCount == static_cast<size_t>(GamepadButton::DPadLeft) + 1);
	static_assert(GamepadAxisCount == static_cast<size_t>(GamepadAxis::RightTrigger) + 1);

	std::string_view KeyToString(Key key)
	{
		return Utils::TryGetKeyName(key);
	}

	std::optional<Key> KeyFromString(std::string_view name)
	{
		return Utils::FindCodeByName<Key>(name, KeyCodeCount, Utils::TryGetKeyName);
	}

	std::string_view MouseButtonToString(MouseButton button)
	{
		return Utils::TryGetMouseButtonName(button);
	}

	std::optional<MouseButton> MouseButtonFromString(std::string_view name)
	{
		return Utils::FindCodeByName<MouseButton>(name, MouseButtonCount, Utils::TryGetMouseButtonName);
	}

	std::string_view GamepadButtonToString(GamepadButton button)
	{
		return Utils::TryGetGamepadButtonName(button);
	}

	std::optional<GamepadButton> GamepadButtonFromString(std::string_view name)
	{
		return Utils::FindCodeByName<GamepadButton>(name, GamepadButtonCount, Utils::TryGetGamepadButtonName);
	}

	std::string_view GamepadAxisToString(GamepadAxis axis)
	{
		return Utils::TryGetGamepadAxisName(axis);
	}

	std::optional<GamepadAxis> GamepadAxisFromString(std::string_view name)
	{
		return Utils::FindCodeByName<GamepadAxis>(name, GamepadAxisCount, Utils::TryGetGamepadAxisName);
	}

}
