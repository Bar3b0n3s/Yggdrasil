#include "EnginePCH.h"
#include "Engine/Platform/Input/KeyCodes.h"

// M2 contract stub (Roadmap rule 3): stream A (window and input) implements the name tables. Until then every name is
// empty and no name parses.

namespace Engine {

	std::string_view KeyToString(Key /*key*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<Key> KeyFromString(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string_view MouseButtonToString(MouseButton /*button*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<MouseButton> MouseButtonFromString(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string_view GamepadButtonToString(GamepadButton /*button*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<GamepadButton> GamepadButtonFromString(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::string_view GamepadAxisToString(GamepadAxis /*axis*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::optional<GamepadAxis> GamepadAxisFromString(std::string_view /*name*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
