#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Events.h"
#include "Engine/Platform/Input/InputActionMap.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The game's input during a play session (Architecture §4.3, §5.7 step 1, §6.7, §13.6): the devices' InputState, the
// project's named actions (InputActionMap) plus injected action states, and the tick-stamped queue every game input goes
// through. One source of truth for automation (play.step input, input.inject), replays and Test.Inject* (M13/M14), and the
// host's real devices: every event is queued for a tick and applied at step 1 of that tick, then the step view is latched,
// so the recorder (input.record, M13) captures exactly the events the simulation saw (§13.6), whatever their source.
//
// Frozen by the M7 contract (Docs/Decisions/0012-m7-decisions.md decision 4). Owned by a PlaySession; main thread only;
// not thread-safe. Deterministic: the views depend only on the sequence of Queue, ApplyTick and LatchFrame calls.

namespace Engine {

	struct InputSettings;

	// The event types of automation input and replays (§6.7, §13.6): registry enum "InputEventType", whose entry names are
	// "Action", "Key", "MouseButton", "MouseMove", "MouseDelta", "Scroll", "GamepadButton", "GamepadAxis" and "Text"
	// (automation accepts any ASCII case, .replay files use these spellings). The enumerators of the four kinds named like a
	// KeyCodes.h type carry the suffix "Input", because GCC's -Wshadow reports an enumerator named like a type.
	enum class PlayInputEventType : uint8_t
	{
		Action,             // Name with State (a button use) or Value (an axis value)
		KeyInput,           // KeyCode with State
		MouseButtonInput,   // Button with State; Position when HasPosition (a MouseMove at Position first)
		MouseMove,          // Position (window coordinates)
		MouseDelta,         // Delta: a MouseMove to the current position + Delta
		Scroll,             // Delta (x right-positive, y up-positive)
		GamepadButtonInput, // Gamepad, GamepadButtonCode with State
		GamepadAxisInput,   // Gamepad, Axis with Value (engine convention: up-positive sticks, §4.3)
		Text                // Text: one CharEvent per code point (no game API reads text before the UI work of later milestones)
	};

	// The state of a button-like event (§13.6): Tap is Down on its tick and Up on the next, so both edges are observed.
	enum class PlayInputEventState : uint8_t
	{
		Down,
		Up,
		Tap
	};

	// One game input event (§6.7 "Events"). Only the members of its Type are read; ValidatePlayInputEvent checks them.
	struct PlayInputEvent
	{
		PlayInputEventType Type = PlayInputEventType::KeyInput;
		std::string Name{};                                     // Action: the project action's name (exact, case-sensitive)
		PlayInputEventState State = PlayInputEventState::Down;  // Action without HasValue, Key, MouseButton, GamepadButton
		bool HasValue = false;                                  // Action: set an axis value (Value) instead of a button state
		float Value = 0.0f;                                     // Action with HasValue: [-1, 1]; GamepadAxis: [-1, 1] sticks, [0, 1] triggers
		Key KeyCode = Key::None;                                // Key
		MouseButton Button = MouseButton::Left;                 // MouseButton
		uint32_t Gamepad = 0;                                   // GamepadButton, GamepadAxis: 0 .. MaxGamepads - 1
		GamepadButton GamepadButtonCode = GamepadButton::South; // GamepadButton
		GamepadAxis Axis = GamepadAxis::LeftX;                  // GamepadAxis
		bool HasPosition = false;                               // MouseButton: Position is given
		glm::vec2 Position{ 0.0f };                             // MouseMove, MouseButton with HasPosition
		glm::vec2 Delta{ 0.0f };                                // MouseDelta, Scroll
		std::string Text{};                                     // Text: UTF-8, at least one code point

		bool operator==(const PlayInputEvent&) const = default;
	};

	// What the game's input looked like in one phase's view (§4.3), for play.state (input) and agents checking that input
	// landed (test_tap_event_seen_once_pressed_and_released): device controls by their binding names ("Key.Space",
	// "Mouse.Left", "Gamepad.South"; a gamepad button counts when any connected gamepad has it) and project actions as
	// "Action.<Name>", each list sorted byte-wise; Axes holds the non-zero values of gamepad axes ("Gamepad.LeftX", the
	// largest magnitude over connected gamepads) and of Axis actions ("Action.MoveX").
	struct PlayInputSummary
	{
		std::vector<std::string> Down{};
		std::vector<std::string> Pressed{};
		std::vector<std::string> Released{};
		std::map<std::string, float> Axes{};

		bool operator==(const PlayInputSummary&) const = default;
	};

	// Checks `event` against `actions` (the members its Type reads). Errors: InvalidArgument naming the member for an
	// unknown action ("did you mean" hint from the action names), a Tap or State on an Action with HasValue, a Value outside
	// its range or not finite, Key::None or a code outside its enumeration, a gamepad index >= MaxGamepads, a non-finite
	// Position or Delta, or empty or invalid UTF-8 Text. Pure.
	[[nodiscard]] Status ValidatePlayInputEvent(const PlayInputEvent& event, const InputActionMap& actions);

	// The game input of one play session (see the file comment).
	//
	// Queue order: events of one tick are applied in the order they were queued (Tap's Up joins the next tick's events in
	// the position of its Queue call). ApplyTick(t) applies the events queued for tick t: device events through
	// InputState::Inject (an injected gamepad event connects its gamepad first when it is not connected; MouseDelta becomes
	// a MouseMove to the current position plus Delta), Action events into the injected action states (Down/Up set the
	// action's injected button and record its edge for both phases; a Value sets its injected axis value); then it latches
	// the step view (InputState::LatchStep and the injected actions' step edges). LatchFrame latches the frame view the same
	// way. Injected action states combine with the action's bindings: an action is down when a binding is down or it was
	// injected down, reports a press when either reports one, and its axis value is the one with the larger magnitude of
	// InputActionMap::GetAxis and the injected value (the injected one on a tie).
	class PlayInput
	{
	public:
		// An empty input: no actions, nothing queued, the next tick 0.
		PlayInput();
		~PlayInput();

		PlayInput(PlayInput&&) noexcept;
		PlayInput& operator=(PlayInput&&) noexcept;
		PlayInput(const PlayInput&) = delete;
		PlayInput& operator=(const PlayInput&) = delete;

		// Builds the input of a session from the project's Input.Actions (§6.1). Errors: Validation from
		// InputActionMap::Create, its issues pointing below "/Input/Actions".
		[[nodiscard]] static Result<PlayInput> Create(const InputSettings& settings);

		// Queues `event` for tick `tick`. Errors: those of ValidatePlayInputEvent, and InvalidArgument when `tick` is before
		// GetNextTick() (input cannot be applied to a tick that already ran); nothing is queued then.
		[[nodiscard]] Status Queue(uint64_t tick, const PlayInputEvent& event);

		// Queues, at `tick` (>= GetNextTick(), asserted) and before the events already queued for it, an Up for every key,
		// mouse button, gamepad button and injected action that is down in the latest queued state, and a zero for every
		// non-zero gamepad axis and injected axis value (input.inject {releaseAll: true}).
		void QueueReleaseAll(uint64_t tick);

		// Converts a real device event of the host (the Runtime's window; the editor's game viewport from M10) and queues it
		// for GetNextTick(): keys, mouse buttons, cursor motion, scrolling, gamepad buttons and axes, and characters.
		// Gamepad connection changes are applied at once instead (they are no replayable input); window and file-drop
		// events are ignored. Continuous sources are coalesced while their tick has not run, so a session that does not
		// advance (paused, or between ticks of a slow FixedHz) holds one event per source instead of one per frame: a cursor
		// position replaces the cursor position queued for that tick, a gamepad axis value the value queued for the same
		// gamepad and axis (each moved to the end of the tick's events), and a scroll offset is added to the tick's last
		// queued scroll. The tick's resulting step view is the same as without coalescing: positions and axis values are
		// absolute, and the step's scroll delta is a sum.
		void QueueDeviceEvent(const Event& event);

		// §5.7 step 1 for tick `tick`, which must equal GetNextTick() (asserted): applies the tick's queued events in order,
		// then latches the step view. GetNextTick() becomes tick + 1.
		void ApplyTick(uint64_t tick);

		// §5.7 frame phase step 1: latches the frame view.
		void LatchFrame();

		// The tick the next ApplyTick applies.
		[[nodiscard]] uint64_t GetNextTick() const;
		// Events queued for ticks that have not run yet.
		[[nodiscard]] size_t GetQueuedEventCount() const;
		// The events the last ApplyTick applied, in order, Tap expanded to Down (and its Up in the next tick's list): what a
		// recording captures for that tick (§13.6).
		[[nodiscard]] std::span<const PlayInputEvent> GetLastAppliedEvents() const;

		[[nodiscard]] const InputState& GetDevices() const;
		[[nodiscard]] const InputActionMap& GetActions() const;

		// The combined action queries of the class comment, for the action `action` (< GetActions().GetActionCount(),
		// asserted) in the view of `phase`.
		[[nodiscard]] bool IsActionDown(InputPhase phase, uint32_t action) const;
		[[nodiscard]] bool WasActionPressed(InputPhase phase, uint32_t action) const;
		[[nodiscard]] bool WasActionReleased(InputPhase phase, uint32_t action) const;
		[[nodiscard]] float GetActionAxis(InputPhase phase, uint32_t action) const;

		// The view of `phase` as a PlayInputSummary.
		[[nodiscard]] PlayInputSummary GetSummary(InputPhase phase) const;
	private:
		// The InputState, the action map, the injected action states and their per-phase edges, the queue by tick, the
		// applied events of the last tick and the next tick (PlayInput.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

	// "Action", "Key", "MouseButton", "MouseMove", "MouseDelta", "Scroll", "GamepadButton", "GamepadAxis" or "Text".
	[[nodiscard]] std::string_view PlayInputEventTypeToString(PlayInputEventType type);
	// "Down", "Up" or "Tap".
	[[nodiscard]] std::string_view PlayInputEventStateToString(PlayInputEventState state);

}
