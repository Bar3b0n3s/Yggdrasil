#include "EnginePCH.h"
#include "Engine/Platform/Input/InputState.h"

#include "Engine/Core/Assert.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// Inject keeps the live state and two pending change sets, one per phase; a latch copies the live state and moves its
// phase's pending changes into that phase's view. An edge is a change of level, so a press of a key that is already
// down, a release of one that is up and a repeat change nothing. A gamepad that is not connected stays at rest: its
// Button and Axis events are ignored until a Connected event arrives (the window always emits Connected first; other
// sources, such as automation from M7, inject one before a gamepad's first control). Axis values are clamped to the
// axis's range and NaN values are ignored, like non-finite cursor positions and scroll offsets, so the views never hold a
// value outside the documented ranges, whatever a caller injects. Deltas and their sums are computed in double, where
// finite floats cannot overflow, and an event whose result would not fit a float is ignored, so finite input never puts
// infinity or NaN into the views either.

namespace Engine {

	namespace Utils {

		static bool IsTriggerAxis(GamepadAxis axis)
		{
			return axis == GamepadAxis::LeftTrigger || axis == GamepadAxis::RightTrigger;
		}

		static size_t GetPhaseIndex(InputPhase phase)
		{
			switch (phase)
			{
				case InputPhase::Step:  return 0;
				case InputPhase::Frame: return 1;
			}

			ENGINE_CORE_ASSERT(false, "Unknown InputPhase {}", std::to_underlying(phase));
			return 1;
		}

		// (x, y) as floats, or nullopt when either is outside the float range (it would become infinity).
		static std::optional<glm::vec2> ToFiniteFloat(double x, double y)
		{
			constexpr double Largest = std::numeric_limits<float>::max();
			if (std::abs(x) > Largest || std::abs(y) > Largest)
				return std::nullopt;
			return glm::vec2(static_cast<float>(x), static_cast<float>(y));
		}

		// Sets bit `index` of `levels` to `isDown` and returns true when it changed (an edge).
		template<size_t Count>
		static bool SetLevel(std::bitset<Count>& levels, size_t index, bool isDown)
		{
			if (levels.test(index) == isDown)
				return false;
			levels.set(index, isDown);
			return true;
		}

	}

	float GamepadAxisValueFromGlfw(GamepadAxis axis, float glfwValue)
	{
		if (std::isnan(glfwValue))
			return 0.0f;

		switch (axis)
		{
			case GamepadAxis::LeftX:
			case GamepadAxis::RightX:
				return std::clamp(glfwValue, -1.0f, 1.0f);
			case GamepadAxis::LeftY:
			case GamepadAxis::RightY:
				// 0 - value rather than -value, so that a centred stick reads +0 and never -0.
				return std::clamp(0.0f - glfwValue, -1.0f, 1.0f);
			case GamepadAxis::LeftTrigger:
			case GamepadAxis::RightTrigger:
				return std::clamp((glfwValue + 1.0f) * 0.5f, 0.0f, 1.0f);
		}

		ENGINE_CORE_ASSERT(false, "Unknown GamepadAxis {}", std::to_underlying(axis));
		return 0.0f;
	}

	void InputState::Inject(const Event& event)
	{
		const auto injectKey = [this](const KeyEvent& key)
		{
			InjectKey(key);
		};
		const auto injectMouseButton = [this](const MouseButtonEvent& button)
		{
			InjectMouseButton(button);
		};
		const auto injectMouseMove = [this](const MouseMoveEvent& move)
		{
			InjectMouseMove(move);
		};
		const auto injectMouseScroll = [this](const MouseScrollEvent& scroll)
		{
			InjectMouseScroll(scroll);
		};
		const auto injectGamepad = [this](const GamepadEvent& gamepad)
		{
			InjectGamepad(gamepad);
		};
		// Window, text and file-drop events carry no input state.
		const auto ignore = [](const auto&) {};
		std::visit(Overloaded{ injectKey, injectMouseButton, injectMouseMove, injectMouseScroll, injectGamepad, ignore }, event);
	}

	void InputState::LatchStep()
	{
		Latch(InputPhase::Step);
	}

	void InputState::LatchFrame()
	{
		Latch(InputPhase::Frame);
	}

	bool InputState::IsKeyDown(InputPhase phase, Key key) const
	{
		const size_t index = std::to_underlying(key);
		return index < KeyCodeCount && GetView(phase).State.KeysDown.test(index);
	}

	bool InputState::WasKeyPressed(InputPhase phase, Key key) const
	{
		const size_t index = std::to_underlying(key);
		return index < KeyCodeCount && GetView(phase).Changes.KeysPressed.test(index);
	}

	bool InputState::WasKeyReleased(InputPhase phase, Key key) const
	{
		const size_t index = std::to_underlying(key);
		return index < KeyCodeCount && GetView(phase).Changes.KeysReleased.test(index);
	}

	bool InputState::IsMouseButtonDown(InputPhase phase, MouseButton button) const
	{
		const size_t index = std::to_underlying(button);
		return index < MouseButtonCount && GetView(phase).State.MouseButtonsDown.test(index);
	}

	bool InputState::WasMouseButtonPressed(InputPhase phase, MouseButton button) const
	{
		const size_t index = std::to_underlying(button);
		return index < MouseButtonCount && GetView(phase).Changes.MouseButtonsPressed.test(index);
	}

	bool InputState::WasMouseButtonReleased(InputPhase phase, MouseButton button) const
	{
		const size_t index = std::to_underlying(button);
		return index < MouseButtonCount && GetView(phase).Changes.MouseButtonsReleased.test(index);
	}

	glm::vec2 InputState::GetMousePosition(InputPhase phase) const
	{
		return GetView(phase).State.MousePosition;
	}

	glm::vec2 InputState::GetMouseDelta(InputPhase phase) const
	{
		return GetView(phase).Changes.MouseDelta;
	}

	glm::vec2 InputState::GetScrollDelta(InputPhase phase) const
	{
		return GetView(phase).Changes.ScrollDelta;
	}

	bool InputState::IsGamepadConnected(InputPhase phase, uint32_t gamepad) const
	{
		return gamepad < MaxGamepads && GetView(phase).State.Gamepads[gamepad].Connected;
	}

	bool InputState::IsGamepadButtonDown(InputPhase phase, uint32_t gamepad, GamepadButton button) const
	{
		const size_t index = std::to_underlying(button);
		return gamepad < MaxGamepads && index < GamepadButtonCount
			&& GetView(phase).State.Gamepads[gamepad].ButtonsDown.test(index);
	}

	bool InputState::WasGamepadButtonPressed(InputPhase phase, uint32_t gamepad, GamepadButton button) const
	{
		const size_t index = std::to_underlying(button);
		return gamepad < MaxGamepads && index < GamepadButtonCount
			&& GetView(phase).Changes.GamepadButtonsPressed[gamepad].test(index);
	}

	bool InputState::WasGamepadButtonReleased(InputPhase phase, uint32_t gamepad, GamepadButton button) const
	{
		const size_t index = std::to_underlying(button);
		return gamepad < MaxGamepads && index < GamepadButtonCount
			&& GetView(phase).Changes.GamepadButtonsReleased[gamepad].test(index);
	}

	float InputState::GetGamepadAxis(InputPhase phase, uint32_t gamepad, GamepadAxis axis) const
	{
		const size_t index = std::to_underlying(axis);
		if (gamepad >= MaxGamepads || index >= GamepadAxisCount)
			return 0.0f;
		return GetView(phase).State.Gamepads[gamepad].Axes[index];
	}

	void InputState::InjectKey(const KeyEvent& key)
	{
		// Key::None and values between the enumerators have no name and are not keys.
		if (KeyToString(key.KeyCode).empty())
			return;
		if (key.Action != ButtonAction::Pressed && key.Action != ButtonAction::Released)
			return;

		const size_t index = std::to_underlying(key.KeyCode);
		const bool isDown = key.Action == ButtonAction::Pressed;
		if (!Utils::SetLevel(m_Live.KeysDown, index, isDown))
			return;
		for (PhaseChanges& changes : m_Pending)
			(isDown ? changes.KeysPressed : changes.KeysReleased).set(index);
	}

	void InputState::InjectMouseButton(const MouseButtonEvent& button)
	{
		const size_t index = std::to_underlying(button.Button);
		if (index >= MouseButtonCount)
			return;
		if (button.Action != ButtonAction::Pressed && button.Action != ButtonAction::Released)
			return;

		const bool isDown = button.Action == ButtonAction::Pressed;
		if (!Utils::SetLevel(m_Live.MouseButtonsDown, index, isDown))
			return;
		for (PhaseChanges& changes : m_Pending)
			(isDown ? changes.MouseButtonsPressed : changes.MouseButtonsReleased).set(index);
	}

	void InputState::InjectMouseMove(const MouseMoveEvent& move)
	{
		if (!std::isfinite(move.Position.x) || !std::isfinite(move.Position.y))
			return;

		// The first position has nothing to move from.
		if (m_HasMousePosition)
		{
			// Every sum is checked before any is changed, so an event is applied to both phases or to neither.
			const double deltaX = static_cast<double>(move.Position.x) - static_cast<double>(m_Live.MousePosition.x);
			const double deltaY = static_cast<double>(move.Position.y) - static_cast<double>(m_Live.MousePosition.y);
			std::array<glm::vec2, PhaseCount> sums{};
			for (size_t phase = 0; phase < PhaseCount; ++phase)
			{
				const glm::vec2 current = m_Pending[phase].MouseDelta;
				const std::optional<glm::vec2> sum = Utils::ToFiniteFloat(static_cast<double>(current.x) + deltaX,
					static_cast<double>(current.y) + deltaY);
				if (!sum.has_value())
					return;
				sums[phase] = *sum;
			}
			for (size_t phase = 0; phase < PhaseCount; ++phase)
				m_Pending[phase].MouseDelta = sums[phase];
		}
		m_Live.MousePosition = move.Position;
		m_HasMousePosition = true;
	}

	void InputState::InjectMouseScroll(const MouseScrollEvent& scroll)
	{
		if (!std::isfinite(scroll.Offset.x) || !std::isfinite(scroll.Offset.y))
			return;

		std::array<glm::vec2, PhaseCount> sums{};
		for (size_t phase = 0; phase < PhaseCount; ++phase)
		{
			const glm::vec2 current = m_Pending[phase].ScrollDelta;
			const std::optional<glm::vec2> sum = Utils::ToFiniteFloat(static_cast<double>(current.x) + static_cast<double>(scroll.Offset.x),
				static_cast<double>(current.y) + static_cast<double>(scroll.Offset.y));
			if (!sum.has_value())
				return;
			sums[phase] = *sum;
		}
		for (size_t phase = 0; phase < PhaseCount; ++phase)
			m_Pending[phase].ScrollDelta = sums[phase];
	}

	void InputState::InjectGamepad(const GamepadEvent& gamepad)
	{
		if (gamepad.Gamepad >= MaxGamepads)
			return;

		GamepadState& state = m_Live.Gamepads[gamepad.Gamepad];
		switch (gamepad.Kind)
		{
			case GamepadEventKind::Connected:
			{
				state.Connected = true;
				return;
			}
			case GamepadEventKind::Disconnected:
			{
				// Every button that was down goes up with a Released edge; the axes return to rest.
				for (PhaseChanges& changes : m_Pending)
					changes.GamepadButtonsReleased[gamepad.Gamepad] |= state.ButtonsDown;
				state = GamepadState{};
				return;
			}
			case GamepadEventKind::Button:
			{
				const size_t index = std::to_underlying(gamepad.Button);
				if (!state.Connected || index >= GamepadButtonCount)
					return;
				if (!Utils::SetLevel(state.ButtonsDown, index, gamepad.Pressed))
					return;
				for (PhaseChanges& changes : m_Pending)
				{
					auto& edges = gamepad.Pressed ? changes.GamepadButtonsPressed : changes.GamepadButtonsReleased;
					edges[gamepad.Gamepad].set(index);
				}
				return;
			}
			case GamepadEventKind::Axis:
			{
				const size_t index = std::to_underlying(gamepad.Axis);
				if (!state.Connected || index >= GamepadAxisCount || std::isnan(gamepad.Value))
					return;
				const float minimum = Utils::IsTriggerAxis(gamepad.Axis) ? 0.0f : -1.0f;
				state.Axes[index] = std::clamp(gamepad.Value, minimum, 1.0f);
				return;
			}
		}
	}

	void InputState::Latch(InputPhase phase)
	{
		const size_t index = Utils::GetPhaseIndex(phase);
		LatchedView& view = m_Views[index];
		view.State = m_Live;
		view.Changes = m_Pending[index];
		m_Pending[index] = PhaseChanges{};
	}

	const InputState::LatchedView& InputState::GetView(InputPhase phase) const
	{
		return m_Views[Utils::GetPhaseIndex(phase)];
	}

}
