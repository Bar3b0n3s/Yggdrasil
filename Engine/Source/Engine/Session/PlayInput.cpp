#include "EnginePCH.h"
#include "Engine/Session/PlayInput.h"

#include "Engine/Core/Assert.h"
#include "Engine/Project/ProjectSettings.h"

#include <utility>

namespace Engine {

	struct PlayInput::State
	{
		InputState Devices;
		InputActionMap Actions;
		std::vector<PlayInputEvent> LastApplied;
	};

	Status ValidatePlayInputEvent(const PlayInputEvent& /*event*/, const InputActionMap& /*actions*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play input validation is not implemented yet (M7 stream A)");
	}

	PlayInput::PlayInput()
		: m_State(CreateScope<State>())
	{
	}

	PlayInput::~PlayInput() = default;
	PlayInput::PlayInput(PlayInput&&) noexcept = default;
	PlayInput& PlayInput::operator=(PlayInput&&) noexcept = default;

	Result<PlayInput> PlayInput::Create(const InputSettings& /*settings*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play input is not implemented yet (M7 stream A)");
	}

	Status PlayInput::Queue(uint64_t /*tick*/, const PlayInputEvent& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "play input is not implemented yet (M7 stream A)");
	}

	void PlayInput::QueueReleaseAll(uint64_t /*tick*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlayInput::QueueDeviceEvent(const Event& /*event*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlayInput::ApplyTick(uint64_t /*tick*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void PlayInput::LatchFrame()
	{
		ENGINE_CONTRACT_STUB();
	}

	uint64_t PlayInput::GetNextTick() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	size_t PlayInput::GetQueuedEventCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::span<const PlayInputEvent> PlayInput::GetLastAppliedEvents() const
	{
		ENGINE_CONTRACT_STUB();
		return m_State->LastApplied;
	}

	const InputState& PlayInput::GetDevices() const
	{
		return m_State->Devices;
	}

	const InputActionMap& PlayInput::GetActions() const
	{
		return m_State->Actions;
	}

	bool PlayInput::IsActionDown(InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool PlayInput::WasActionPressed(InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool PlayInput::WasActionReleased(InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	float PlayInput::GetActionAxis(InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	PlayInputSummary PlayInput::GetSummary(InputPhase /*phase*/) const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::string_view PlayInputEventTypeToString(PlayInputEventType type)
	{
		switch (type)
		{
			case PlayInputEventType::Action:
				return "Action";
			case PlayInputEventType::KeyInput:
				return "Key";
			case PlayInputEventType::MouseButtonInput:
				return "MouseButton";
			case PlayInputEventType::MouseMove:
				return "MouseMove";
			case PlayInputEventType::MouseDelta:
				return "MouseDelta";
			case PlayInputEventType::Scroll:
				return "Scroll";
			case PlayInputEventType::GamepadButtonInput:
				return "GamepadButton";
			case PlayInputEventType::GamepadAxisInput:
				return "GamepadAxis";
			case PlayInputEventType::Text:
				return "Text";
		}
		ENGINE_CORE_ASSERT(false, "unknown PlayInputEventType");
		return "Unknown";
	}

	std::string_view PlayInputEventStateToString(PlayInputEventState state)
	{
		switch (state)
		{
			case PlayInputEventState::Down:
				return "Down";
			case PlayInputEventState::Up:
				return "Up";
			case PlayInputEventState::Tap:
				return "Tap";
		}
		ENGINE_CORE_ASSERT(false, "unknown PlayInputEventState");
		return "Unknown";
	}

}
