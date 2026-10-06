#include "EnginePCH.h"
#include "Engine/Platform/Input/InputState.h"

// M2 contract stub (Roadmap rule 3): stream A (window and input) implements injection, latching and the queries. Until
// then nothing is recorded and every query reads up, unconnected and zero.

namespace Engine {

	float GamepadAxisValueFromGlfw(GamepadAxis /*axis*/, float /*glfwValue*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	void InputState::Inject(const Event& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void InputState::LatchStep()
	{
		ENGINE_CONTRACT_STUB();
	}

	void InputState::LatchFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	bool InputState::IsKeyDown(InputPhase /*phase*/, Key /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasKeyPressed(InputPhase /*phase*/, Key /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasKeyReleased(InputPhase /*phase*/, Key /*key*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::IsMouseButtonDown(InputPhase /*phase*/, MouseButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasMouseButtonPressed(InputPhase /*phase*/, MouseButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasMouseButtonReleased(InputPhase /*phase*/, MouseButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	glm::vec2 InputState::GetMousePosition(InputPhase /*phase*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	glm::vec2 InputState::GetMouseDelta(InputPhase /*phase*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	glm::vec2 InputState::GetScrollDelta(InputPhase /*phase*/) const
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec2(0.0f);
	}

	bool InputState::IsGamepadConnected(InputPhase /*phase*/, uint32_t /*gamepad*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::IsGamepadButtonDown(InputPhase /*phase*/, uint32_t /*gamepad*/, GamepadButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasGamepadButtonPressed(InputPhase /*phase*/, uint32_t /*gamepad*/, GamepadButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputState::WasGamepadButtonReleased(InputPhase /*phase*/, uint32_t /*gamepad*/, GamepadButton /*button*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	float InputState::GetGamepadAxis(InputPhase /*phase*/, uint32_t /*gamepad*/, GamepadAxis /*axis*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

}
