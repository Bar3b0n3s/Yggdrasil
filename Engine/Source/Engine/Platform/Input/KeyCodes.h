#pragma once

#include "Engine/Core/Base.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// Input device codes (Architecture §4.3) and their names. The names are what files, replays, automation and scripts use
// ("Space", "Left", "South", "LeftX"; binding names add a device prefix, InputActionMap.h). The numeric values equal
// GLFW's (GLFW_KEY_*, GLFW_MOUSE_BUTTON_*, GLFW_GAMEPAD_BUTTON_*, GLFW_GAMEPAD_AXIS_*, GLFW_MOD_*), so the window converts
// GLFW codes with a range check and a cast. The values are never serialized.

namespace Engine {

	// Keyboard keys, named after their position on a US layout (GLFW key codes). None is no key.
	enum class Key : uint16_t
	{
		None = 0,

		Space = 32,
		Apostrophe = 39,
		Comma = 44,
		Minus = 45,
		Period = 46,
		Slash = 47,
		D0 = 48,
		D1 = 49,
		D2 = 50,
		D3 = 51,
		D4 = 52,
		D5 = 53,
		D6 = 54,
		D7 = 55,
		D8 = 56,
		D9 = 57,
		Semicolon = 59,
		Equal = 61,
		A = 65,
		B = 66,
		C = 67,
		D = 68,
		E = 69,
		F = 70,
		G = 71,
		H = 72,
		I = 73,
		J = 74,
		K = 75,
		L = 76,
		M = 77,
		N = 78,
		O = 79,
		P = 80,
		Q = 81,
		R = 82,
		S = 83,
		T = 84,
		U = 85,
		V = 86,
		W = 87,
		X = 88,
		Y = 89,
		Z = 90,
		LeftBracket = 91,
		Backslash = 92,
		RightBracket = 93,
		GraveAccent = 96,
		World1 = 161,
		World2 = 162,

		Escape = 256,
		Enter = 257,
		Tab = 258,
		Backspace = 259,
		Insert = 260,
		Delete = 261,
		Right = 262,
		Left = 263,
		Down = 264,
		Up = 265,
		PageUp = 266,
		PageDown = 267,
		Home = 268,
		End = 269,
		CapsLock = 280,
		ScrollLock = 281,
		NumLock = 282,
		PrintScreen = 283,
		Pause = 284,
		F1 = 290,
		F2 = 291,
		F3 = 292,
		F4 = 293,
		F5 = 294,
		F6 = 295,
		F7 = 296,
		F8 = 297,
		F9 = 298,
		F10 = 299,
		F11 = 300,
		F12 = 301,
		F13 = 302,
		F14 = 303,
		F15 = 304,
		F16 = 305,
		F17 = 306,
		F18 = 307,
		F19 = 308,
		F20 = 309,
		F21 = 310,
		F22 = 311,
		F23 = 312,
		F24 = 313,
		F25 = 314,
		Keypad0 = 320,
		Keypad1 = 321,
		Keypad2 = 322,
		Keypad3 = 323,
		Keypad4 = 324,
		Keypad5 = 325,
		Keypad6 = 326,
		Keypad7 = 327,
		Keypad8 = 328,
		Keypad9 = 329,
		KeypadDecimal = 330,
		KeypadDivide = 331,
		KeypadMultiply = 332,
		KeypadSubtract = 333,
		KeypadAdd = 334,
		KeypadEnter = 335,
		KeypadEqual = 336,
		LeftShift = 340,
		LeftControl = 341,
		LeftAlt = 342,
		LeftSuper = 343,
		RightShift = 344,
		RightControl = 345,
		RightAlt = 346,
		RightSuper = 347,
		Menu = 348
	};

	// One past the largest Key value: the size of a table indexed by Key. Values below it that name no enumerator (33,
	// 100, ...) are not keys.
	inline constexpr size_t KeyCodeCount = static_cast<size_t>(Key::Menu) + 1;

	// Mouse buttons (GLFW_MOUSE_BUTTON_1 to _8).
	enum class MouseButton : uint8_t
	{
		Left = 0,
		Right = 1,
		Middle = 2,
		Button4 = 3,
		Button5 = 4,
		Button6 = 5,
		Button7 = 6,
		Button8 = 7
	};

	inline constexpr size_t MouseButtonCount = 8;

	// Gamepad buttons by position, as GLFW's gamepad mappings (SDL_GameControllerDB) name them: South is A on an Xbox pad
	// and Cross on a PlayStation pad, East is B / Circle, West is X / Square, North is Y / Triangle.
	enum class GamepadButton : uint8_t
	{
		South = 0,
		East = 1,
		West = 2,
		North = 3,
		LeftBumper = 4,
		RightBumper = 5,
		Back = 6,
		Start = 7,
		Guide = 8,
		LeftThumb = 9,
		RightThumb = 10,
		DPadUp = 11,
		DPadRight = 12,
		DPadDown = 13,
		DPadLeft = 14
	};

	inline constexpr size_t GamepadButtonCount = 15;

	// Gamepad axes. Engine values: the sticks go from -1 to 1, right-positive and up-positive (GLFW's stick Y axes are
	// positive-down and are negated when read); the triggers go from 0 (released) to 1 (fully pressed), where GLFW reports
	// -1 to 1 and the value is remapped when read (GamepadAxisValueFromGlfw, InputState.h). With 0 as the rest value of
	// every axis, a released trigger, an unconnected gamepad and the dead zone all read 0. Replays and automation store
	// engine values (§6.7, §13.6), so this range is part of their format.
	enum class GamepadAxis : uint8_t
	{
		LeftX = 0,
		LeftY = 1,
		RightX = 2,
		RightY = 3,
		LeftTrigger = 4,
		RightTrigger = 5
	};

	inline constexpr size_t GamepadAxisCount = 6;

	// Modifier keys held during a key or mouse-button event (GLFW_MOD_*). CapsLock and NumLock report the lock state.
	enum class KeyModifiers : uint8_t
	{
		None = 0,
		Shift = 1 << 0,
		Control = 1 << 1,
		Alt = 1 << 2,
		Super = 1 << 3,
		CapsLock = 1 << 4,
		NumLock = 1 << 5
	};

	template<>
	inline constexpr bool EnableFlagOperators<KeyModifiers> = true;

	// What happened to a key or button (GLFW_RELEASE, GLFW_PRESS, GLFW_REPEAT). Only keys repeat.
	enum class ButtonAction : uint8_t
	{
		Released = 0,
		Pressed = 1,
		Repeated = 2
	};

	// The enumerator name ("Space", "D0", "Keypad0", "LeftShift"); empty for Key::None and for values that name no key.
	[[nodiscard]] std::string_view KeyToString(Key key);
	// The key named `name`, compared ASCII case-insensitively ("space" and "SPACE" are Key::Space); nullopt for an unknown
	// name and for "None".
	[[nodiscard]] std::optional<Key> KeyFromString(std::string_view name);

	// "Left", "Right", "Middle", "Button4" ... "Button8"; empty for a value outside the enumeration.
	[[nodiscard]] std::string_view MouseButtonToString(MouseButton button);
	// ASCII case-insensitive inverse of MouseButtonToString; nullopt for an unknown name.
	[[nodiscard]] std::optional<MouseButton> MouseButtonFromString(std::string_view name);

	// "South", "East", ... "DPadLeft"; empty for a value outside the enumeration.
	[[nodiscard]] std::string_view GamepadButtonToString(GamepadButton button);
	// ASCII case-insensitive inverse of GamepadButtonToString; nullopt for an unknown name.
	[[nodiscard]] std::optional<GamepadButton> GamepadButtonFromString(std::string_view name);

	// "LeftX", "LeftY", "RightX", "RightY", "LeftTrigger", "RightTrigger"; empty for a value outside the enumeration.
	[[nodiscard]] std::string_view GamepadAxisToString(GamepadAxis axis);
	// ASCII case-insensitive inverse of GamepadAxisToString; nullopt for an unknown name.
	[[nodiscard]] std::optional<GamepadAxis> GamepadAxisFromString(std::string_view name);

}
