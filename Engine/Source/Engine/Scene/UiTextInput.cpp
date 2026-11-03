#include "EnginePCH.h"
#include "Engine/Scene/UiTextInput.h"

// The text input's editing (UiTextInput.h): stubs of the M22 contract (C22.3, Docs/Decisions/0033-m22-contract.md decision
// 47) that stream T implements.

namespace Engine {

	uint32_t ReadUiTextInputCaret(const UiTextInputComponent& /*input*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t GetUiTextInputCaretByte(const UiTextInputComponent& /*input*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	bool EditUiTextInput(Scene& /*scene*/, UUID /*input*/, std::string_view /*typed*/, UiTextKeys /*keys*/, std::vector<UiEvent>& /*events*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

}
