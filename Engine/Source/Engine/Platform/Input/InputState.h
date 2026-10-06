#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Platform/Events.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <glm/vec2.hpp>

#include <cstdint>

// Input state (Architecture §4.3): keys, mouse buttons, cursor, scroll and up to 4 gamepads, with phase-aware edges.

namespace Engine {

	inline constexpr uint32_t MaxGamepads = 4;

	// The latched view a query reads (§4.3). Step: inside fixed steps (script OnFixedUpdate, physics callbacks). Frame:
	// inside the frame phase (script OnUpdate and OnLateUpdate, editor camera).
	enum class InputPhase : uint8_t
	{
		Step,
		Frame
	};

	// Converts a gamepad axis value read from GLFW to the engine convention (§4.3, KeyCodes.h): GLFW reports the stick Y
	// axes positive-down, so LeftY and RightY are negated, and the triggers from -1 (released) to 1, so LeftTrigger and
	// RightTrigger become (value + 1) / 2; LeftX and RightX pass through. The result is clamped to [-1, 1] for sticks and
	// [0, 1] for triggers. This is the one place the convention changes: the window applies it to every polled axis before
	// emitting a GamepadEvent, and every other source (automation, replays, tests) already uses the engine convention.
	[[nodiscard]] float GamepadAxisValueFromGlfw(GamepadAxis axis, float glfwValue);

	// Plain input data owned by EngineContext (§4.3). InputState::Inject is the only write path: GLFW callbacks (through
	// the window and the frame loop), automation and tests all go through it, so injected and real input behave the same.
	//
	// Phase-aware edges. Inject updates the live state at once and records every edge (a key, mouse button or gamepad
	// button going down or up), cursor motion and scrolling into two pending accumulators, one per phase. Each phase has
	// a latched view that queries read:
	//   - LatchStep() runs at the start of every fixed step, before that step's simulation code. It copies the live state
	//     into the Step view and moves the Step accumulator into it, so the view reports exactly the edges since the
	//     previous LatchStep.
	//   - LatchFrame() runs once per frame before OnUpdate and does the same for the Frame view.
	// Consequences, each covered by a Roadmap M2 test:
	//   - a tap (down and up) between two steps reports both Pressed and Released on the next step, with IsDown false;
	//   - frames that run zero steps keep the step edges pending until the next step;
	//   - frames that run several steps report an edge on the first of them only;
	//   - the Frame view's edges are independent of how many steps ran.
	// Nothing calls the latch functions on its own: the simulation that reads the views calls them at the points §5.7
	// gives (PlaySession, M7: LatchStep after applying the events stamped for the tick, LatchFrame at the start of the
	// frame phase). The frame loop only injects.
	//
	// Queries are total: a code outside its enumeration (Key::None, a cast value) or a gamepad index >= MaxGamepads reads
	// as up, unconnected and zero, and Inject ignores events that carry one; callers that take codes from scripts or
	// automation validate them first to report the error. Repeated key events change nothing. CharEvent, FileDropEvent
	// and window events do not change the input state.
	//
	// Cursor: the position of the last MouseMoveEvent; the first one sets the position without producing a delta. A
	// view's mouse delta and scroll delta are the sums since that phase's previous latch. Gamepads: Connected and
	// Disconnected events change the connection state; Disconnected also releases every button (with Released edges)
	// and zeroes the axes, which is the rest value of every axis (sticks centred, triggers released). Axis values are the
	// last injected value (the dead zone is applied by InputActionMap).
	//
	// Not thread-safe: the main thread owns it. Deterministic: the views depend only on the sequence of Inject and latch
	// calls.
	class InputState
	{
	public:
		InputState() = default;

		void Inject(const Event& event);

		// Latches the Step view (see the class comment).
		void LatchStep();

		// Latches the Frame view (see the class comment).
		void LatchFrame();

		// Keys: down at the phase's last latch; pressed or released since the latch before it.
		[[nodiscard]] bool IsKeyDown(InputPhase phase, Key key) const;
		[[nodiscard]] bool WasKeyPressed(InputPhase phase, Key key) const;
		[[nodiscard]] bool WasKeyReleased(InputPhase phase, Key key) const;

		[[nodiscard]] bool IsMouseButtonDown(InputPhase phase, MouseButton button) const;
		[[nodiscard]] bool WasMouseButtonPressed(InputPhase phase, MouseButton button) const;
		[[nodiscard]] bool WasMouseButtonReleased(InputPhase phase, MouseButton button) const;

		// The cursor position at the phase's last latch (window coordinates).
		[[nodiscard]] glm::vec2 GetMousePosition(InputPhase phase) const;
		// The cursor motion between the phase's last two latches.
		[[nodiscard]] glm::vec2 GetMouseDelta(InputPhase phase) const;
		// The scrolling between the phase's last two latches (x right-positive, y up-positive).
		[[nodiscard]] glm::vec2 GetScrollDelta(InputPhase phase) const;

		[[nodiscard]] bool IsGamepadConnected(InputPhase phase, uint32_t gamepad) const;
		[[nodiscard]] bool IsGamepadButtonDown(InputPhase phase, uint32_t gamepad, GamepadButton button) const;
		[[nodiscard]] bool WasGamepadButtonPressed(InputPhase phase, uint32_t gamepad, GamepadButton button) const;
		[[nodiscard]] bool WasGamepadButtonReleased(InputPhase phase, uint32_t gamepad, GamepadButton button) const;
		// The axis value at the phase's last latch, engine convention (up-positive sticks), without dead zone.
		[[nodiscard]] float GetGamepadAxis(InputPhase phase, uint32_t gamepad, GamepadAxis axis) const;
	};

}
