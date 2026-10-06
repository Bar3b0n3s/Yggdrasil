#include "TestsPCH.h"

#include "Engine/Platform/Input/KeyCodes.h"

namespace Engine {

	TEST_SUITE("Platform")
	{
		TEST_CASE("KeyCodes: values match GLFW's key, button and modifier codes")
		{
			// The window converts GLFW codes with a cast (KeyCodes.h), so these values are part of the contract.
			CHECK(std::to_underlying(Key::Space) == 32);
			CHECK(std::to_underlying(Key::A) == 65);
			CHECK(std::to_underlying(Key::Escape) == 256);
			CHECK(std::to_underlying(Key::F25) == 314);
			CHECK(std::to_underlying(Key::Keypad0) == 320);
			CHECK(std::to_underlying(Key::Menu) == 348);
			CHECK(KeyCodeCount == 349);
			CHECK(std::to_underlying(MouseButton::Button8) == MouseButtonCount - 1);
			CHECK(std::to_underlying(GamepadButton::DPadLeft) == GamepadButtonCount - 1);
			CHECK(std::to_underlying(GamepadAxis::RightTrigger) == GamepadAxisCount - 1);
			CHECK(std::to_underlying(KeyModifiers::NumLock) == 0x20);
			CHECK(std::to_underlying(ButtonAction::Repeated) == 2);

			const KeyModifiers modifiers = KeyModifiers::Shift | KeyModifiers::Control;
			CHECK(HasFlag(modifiers, KeyModifiers::Control));
			CHECK_FALSE(HasFlag(modifiers, KeyModifiers::Alt));
		}

		TEST_CASE("KeyCodes: every key name round-trips and parsing ignores ASCII case")
		{
			size_t keyCount = 0;
			for (size_t value = 1; value < KeyCodeCount; ++value)
			{
				const Key key = static_cast<Key>(value);
				const std::string_view name = KeyToString(key);
				if (name.empty())
					continue; // not a key (a gap in the GLFW code range)
				++keyCount;
				INFO("key name: ", std::string(name));
				const std::optional<Key> parsed = KeyFromString(name);
				REQUIRE(parsed.has_value());
				CHECK(*parsed == key);
			}
			CHECK(keyCount == 120);

			CHECK(KeyToString(Key::Space) == "Space");
			CHECK(KeyToString(Key::D0) == "D0");
			CHECK(KeyToString(Key::KeypadEnter) == "KeypadEnter");
			CHECK(KeyFromString("space") == Key::Space);
			CHECK(KeyFromString("LEFTSHIFT") == Key::LeftShift);
			CHECK(KeyToString(Key::None).empty());
			CHECK(KeyToString(static_cast<Key>(33)).empty());
			CHECK_FALSE(KeyFromString("None").has_value());
			CHECK_FALSE(KeyFromString("").has_value());
			CHECK_FALSE(KeyFromString("Spacebar").has_value());
		}

		TEST_CASE("KeyCodes: mouse buttons, gamepad buttons and gamepad axes round-trip by name")
		{
			for (size_t index = 0; index < MouseButtonCount; ++index)
			{
				const MouseButton button = static_cast<MouseButton>(index);
				CHECK(MouseButtonFromString(MouseButtonToString(button)) == button);
			}
			for (size_t index = 0; index < GamepadButtonCount; ++index)
			{
				const GamepadButton button = static_cast<GamepadButton>(index);
				CHECK(GamepadButtonFromString(GamepadButtonToString(button)) == button);
			}
			for (size_t index = 0; index < GamepadAxisCount; ++index)
			{
				const GamepadAxis axis = static_cast<GamepadAxis>(index);
				CHECK(GamepadAxisFromString(GamepadAxisToString(axis)) == axis);
			}

			CHECK(MouseButtonToString(MouseButton::Left) == "Left");
			CHECK(GamepadButtonToString(GamepadButton::South) == "South");
			CHECK(GamepadAxisToString(GamepadAxis::LeftY) == "LeftY");
			CHECK(GamepadButtonFromString("dpadleft") == GamepadButton::DPadLeft);
			CHECK(GamepadAxisFromString("RIGHTTRIGGER") == GamepadAxis::RightTrigger);
			CHECK_FALSE(MouseButtonFromString("Button9").has_value());
			CHECK_FALSE(GamepadButtonFromString("A").has_value());
			CHECK_FALSE(GamepadAxisFromString("LeftZ").has_value());
			CHECK(MouseButtonToString(static_cast<MouseButton>(MouseButtonCount)).empty());
		}
	}

}
