#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Platform/Input/InputState.h"
#include "Engine/Platform/Input/KeyCodes.h"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// Named input actions (Architecture §4.3, project setting Input.Actions §6.1): Button actions and Axis actions resolved
// against an InputState view.

namespace Engine {

	// One control named by a binding (§4.3): "Key.<Key>", "Mouse.<MouseButton>", "Gamepad.<GamepadButton>" or
	// "Gamepad.<GamepadAxis>", with the names of KeyCodes.h ("Key.Space", "Mouse.Left", "Gamepad.South", "Gamepad.LeftX").
	struct InputBinding
	{
		using Control = std::variant<Key, MouseButton, GamepadButton, GamepadAxis>;

		Control Target = Key::None;

		// Parses a binding name. The device prefix and the control name are compared ASCII case-insensitively, as
		// automation enum values are (§13.4); ToString gives the canonical spelling. Errors: Validation naming the binding
		// when the prefix or the control is unknown (Key.None included).
		[[nodiscard]] static Result<InputBinding> Parse(std::string_view name);

		// The canonical binding name ("Key.Space"); empty for Key::None or a value outside its enumeration.
		[[nodiscard]] std::string ToString() const;

		[[nodiscard]] bool operator==(const InputBinding& other) const = default;
	};

	enum class InputActionType : uint8_t
	{
		Button, // down while any binding is down
		Axis    // a value in [-1, 1] from Positive/Negative bindings and an optional gamepad axis
	};

	// One action as the project defines it (§6.1 Input.Actions; Name is the map key). Binding names are validated by
	// InputActionMap::Create.
	struct InputActionDefinition
	{
		std::string Name{};
		InputActionType Type = InputActionType::Button;
		std::vector<std::string> Bindings{}; // Button: keys, mouse buttons or gamepad buttons
		std::vector<std::string> Positive{}; // Axis: keys, mouse buttons or gamepad buttons that give +1
		std::vector<std::string> Negative{}; // Axis: ... that give -1
		std::string Gamepad{};               // Axis: an optional gamepad axis ("Gamepad.LeftY"); empty for none
		bool Invert = false;                 // Axis: flips the gamepad axis (not the Positive/Negative bindings)
	};

	// Resolves named actions against InputState (§4.3). A value type built once from the project's definitions; the
	// queries are pure functions of the definitions and the InputState view, so they are deterministic and phase-aware
	// exactly like InputState. Thread-compatible (const queries may run concurrently).
	//
	// Every query works for both action types:
	//   - Button action: IsDown while any binding is down. WasPressed / WasReleased when any binding reported that edge in
	//     the view (the union of the bindings' edges, so pressing a second binding while the first is held reports a
	//     second press). GetAxis is 1 while down, else 0.
	//   - Axis action: GetAxis combines two values and returns the one with the larger magnitude (the key value on a tie):
	//       keys:    (any Positive binding down ? 1 : 0) - (any Negative binding down ? 1 : 0);
	//       gamepad: the bound axis of every connected gamepad, negated when Invert is set, through ApplyDeadZone; of these
	//                the value with the largest magnitude (the lowest gamepad index on a tie), 0 with no gamepad.
	//     IsDown, WasPressed and WasReleased treat its Positive and Negative bindings as the bindings of a Button action
	//     (the gamepad axis is ignored).
	// Gamepad button bindings likewise read every connected gamepad.
	class InputActionMap
	{
	public:
		// Gamepad axis values with a magnitude up to this are 0 (§4.3).
		static constexpr float DeadZone = 0.15f;

		// An empty map: FindAction finds nothing.
		InputActionMap() = default;

		// Validates `definitions` and builds the map. Actions are indexed in byte-wise name order, whatever the order of
		// `definitions`. Errors: Validation carrying one ErrorIssue per problem, with the pointer "/<Name>/<Field>[/<i>]":
		// an empty or duplicated name; an unknown binding name; a gamepad axis among Bindings, Positive or Negative; a
		// Gamepad that is not a gamepad axis; Bindings on an Axis action; Positive, Negative, Gamepad or Invert on a
		// Button action.
		[[nodiscard]] static Result<InputActionMap> Create(std::span<const InputActionDefinition> definitions);

		// The index of the action named `name` (exact, case-sensitive); nullopt when there is none. Scripts report an
		// unknown name as INPUT_UNKNOWN_ACTION (M13).
		[[nodiscard]] std::optional<uint32_t> FindAction(std::string_view name) const;

		[[nodiscard]] uint32_t GetActionCount() const;

		// The definition of action `action` (< GetActionCount(), asserted).
		[[nodiscard]] const InputActionDefinition& GetDefinition(uint32_t action) const;

		// The queries of the class comment. `action` must be < GetActionCount() (asserted).
		[[nodiscard]] bool IsDown(const InputState& input, InputPhase phase, uint32_t action) const;
		[[nodiscard]] bool WasPressed(const InputState& input, InputPhase phase, uint32_t action) const;
		[[nodiscard]] bool WasReleased(const InputState& input, InputPhase phase, uint32_t action) const;
		[[nodiscard]] float GetAxis(const InputState& input, InputPhase phase, uint32_t action) const;

		// The dead zone of gamepad axes: 0 when |value| <= DeadZone, otherwise rescaled linearly so that the output runs
		// from 0 at the dead zone's edge to ±1 at ±1: sign(value) * (|value| - DeadZone) / (1 - DeadZone). `value` is
		// clamped to [-1, 1] first.
		[[nodiscard]] static float ApplyDeadZone(float value);
	};

}
