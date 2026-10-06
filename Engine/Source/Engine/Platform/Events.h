#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Window and input events (Architecture §4.3): plain structs in one std::variant, handled with std::visit and the
// Overloaded helper. There is no event class hierarchy and no event bus. The window queues the events GLFW reports and the
// ones injected through Window::InjectEvent, and delivers them in order during PollEvents; the frame loop passes each one
// to Application::OnEvent and, unless the handler marked it handled, to InputState::Inject (§4.2 step 1). Automation,
// replays and tests produce the same events, so every source shares one path.
//
// Every alternative carries a Handled flag. Coordinates are window coordinates (screen units, origin at the top-left
// corner, y down); sizes are separate for the window and its framebuffer, which differ on Retina and scaled displays.

namespace Engine {

	// The user asked to close the window (close button, Alt+F4). Unhandled, it makes the application exit with Success.
	struct WindowCloseEvent
	{
		bool Handled = false;
	};

	// The window or its framebuffer changed size. Both sizes are current; a framebuffer of 0x0 means minimized.
	struct WindowResizeEvent
	{
		uint32_t Width = 0; // window size in screen units
		uint32_t Height = 0;
		uint32_t FramebufferWidth = 0; // framebuffer size in pixels
		uint32_t FramebufferHeight = 0;
		bool Handled = false;
	};

	// The window gained or lost input focus.
	struct WindowFocusEvent
	{
		bool Focused = false;
		bool Handled = false;
	};

	struct KeyEvent
	{
		Key KeyCode = Key::None;
		ButtonAction Action = ButtonAction::Pressed;
		KeyModifiers Modifiers = KeyModifiers::None;
		bool Handled = false;
	};

	// One Unicode code point of text input (after keyboard layout and dead keys), for text fields.
	struct CharEvent
	{
		uint32_t Codepoint = 0;
		bool Handled = false;
	};

	struct MouseButtonEvent
	{
		MouseButton Button = MouseButton::Left;
		ButtonAction Action = ButtonAction::Pressed; // Pressed or Released
		KeyModifiers Modifiers = KeyModifiers::None;
		bool Handled = false;
	};

	// The cursor moved to Position (window coordinates). With CursorMode::Locked the position is unbounded and only its
	// change is meaningful.
	struct MouseMoveEvent
	{
		glm::vec2 Position{ 0.0f };
		bool Handled = false;
	};

	// Scroll wheel or touchpad scrolling: x is right-positive, y is up-positive (the wheel turned away from the user).
	struct MouseScrollEvent
	{
		glm::vec2 Offset{ 0.0f };
		bool Handled = false;
	};

	// Files dropped onto the window: absolute paths, UTF-8, in the order the OS reported them.
	struct FileDropEvent
	{
		std::vector<std::string> Paths{};
		bool Handled = false;
	};

	enum class GamepadEventKind : uint8_t
	{
		Connected,    // a gamepad appeared at index Gamepad
		Disconnected, // it went away; InputState releases its buttons and zeroes its axes
		Button,       // Button changed to Pressed
		Axis          // Axis changed to Value
	};

	// A change of one gamepad (§4.3: up to 4 gamepads, through GLFW's gamepad mappings). GLFW reports gamepads by polling,
	// so the window compares each polled state with the previous one and emits one event per change. Axis values follow
	// the engine convention (KeyCodes.h): sticks from -1 to 1, right-positive and up-positive, triggers from 0 released to
	// 1 pressed (GamepadAxisValueFromGlfw, InputState.h). Automation and replays inject the same events (gamepadButton,
	// gamepadAxis, §13.6).
	struct GamepadEvent
	{
		uint32_t Gamepad = 0; // 0 .. MaxGamepads - 1 (InputState.h)
		GamepadEventKind Kind = GamepadEventKind::Button;
		GamepadButton Button = GamepadButton::South; // Kind == Button
		bool Pressed = false;                        // Kind == Button
		GamepadAxis Axis = GamepadAxis::LeftX;       // Kind == Axis
		float Value = 0.0f;                          // Kind == Axis: in [-1, 1] for sticks, [0, 1] for triggers
		bool Handled = false;
	};

	using Event = std::variant<WindowCloseEvent, WindowResizeEvent, WindowFocusEvent, KeyEvent, CharEvent, MouseButtonEvent,
		MouseMoveEvent, MouseScrollEvent, FileDropEvent, GamepadEvent>;

	// Combines lambdas into one visitor for std::visit:
	//     std::visit(Overloaded{ [](const KeyEvent& key) { ... }, [](const auto&) {} }, event);
	template<typename... Functions>
	struct Overloaded : Functions...
	{
		using Functions::operator()...;
	};

	template<typename... Functions>
	Overloaded(Functions...) -> Overloaded<Functions...>;

	// The Handled flag of whichever alternative `event` holds.
	[[nodiscard]] inline bool IsHandled(const Event& event)
	{
		return std::visit([](const auto& alternative)
		{
			return alternative.Handled;
		}, event);
	}

	// Sets the Handled flag of whichever alternative `event` holds.
	inline void SetHandled(Event& event, bool handled = true)
	{
		std::visit([handled](auto& alternative)
		{
			alternative.Handled = handled;
		}, event);
	}

	// The alternative's type name ("KeyEvent"), for logs and diagnostics.
	[[nodiscard]] inline std::string_view GetEventName(const Event& event)
	{
		// In the order of the Event alternatives.
		constexpr std::array<std::string_view, std::variant_size_v<Event>> Names = {
			"WindowCloseEvent",
			"WindowResizeEvent",
			"WindowFocusEvent",
			"KeyEvent",
			"CharEvent",
			"MouseButtonEvent",
			"MouseMoveEvent",
			"MouseScrollEvent",
			"FileDropEvent",
			"GamepadEvent",
		};
		return Names[event.index()];
	}

}
