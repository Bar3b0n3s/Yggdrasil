#include "EnginePCH.h"
#include "Engine/Platform/Input/InputActionMap.h"

// M2 contract stub (Roadmap rule 3): stream A (window and input) implements binding parsing, validation and the action
// queries. Until then parsing and Create fail with Unsupported, and every query reads up and zero.

namespace Engine {

	Result<InputBinding> InputBinding::Parse(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "InputBinding::Parse is not implemented yet");
	}

	std::string InputBinding::ToString() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<InputActionMap> InputActionMap::Create(std::span<const InputActionDefinition> /*definitions*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "InputActionMap::Create is not implemented yet");
	}

	std::optional<uint32_t> InputActionMap::FindAction(std::string_view /*name*/) const
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	uint32_t InputActionMap::GetActionCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	const InputActionDefinition& InputActionMap::GetDefinition(uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		static const InputActionDefinition EmptyDefinition;
		return EmptyDefinition;
	}

	bool InputActionMap::IsDown(const InputState& /*input*/, InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputActionMap::WasPressed(const InputState& /*input*/, InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	bool InputActionMap::WasReleased(const InputState& /*input*/, InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	float InputActionMap::GetAxis(const InputState& /*input*/, InputPhase /*phase*/, uint32_t /*action*/) const
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

	float InputActionMap::ApplyDeadZone(float /*value*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0.0f;
	}

}
