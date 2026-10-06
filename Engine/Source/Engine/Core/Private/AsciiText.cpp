#include "EnginePCH.h"
#include "Engine/Core/Private/AsciiText.h"

namespace Engine {

	namespace Utils {

		char ToAsciiLower(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		bool EqualsIgnoreAsciiCase(std::string_view lhs, std::string_view rhs)
		{
			return std::ranges::equal(lhs, rhs, [](char left, char right)
			{
				return ToAsciiLower(left) == ToAsciiLower(right);
			});
		}

	}

}
