#include "EnginePCH.h"
#include "Engine/Core/UUID.h"

namespace Engine {

	namespace Utils {

		static constexpr std::string_view HexDigits = "0123456789abcdef";

		// The value of one hexadecimal digit of either case, or nullopt.
		static std::optional<uint64_t> HexDigitValue(char character)
		{
			if (character >= '0' && character <= '9')
				return static_cast<uint64_t>(character - '0');
			if (character >= 'a' && character <= 'f')
				return static_cast<uint64_t>(character - 'a' + 10);
			if (character >= 'A' && character <= 'F')
				return static_cast<uint64_t>(character - 'A' + 10);
			return std::nullopt;
		}

		static bool IsHexText(std::string_view text)
		{
			return std::ranges::all_of(text, [](char character)
			{
				return HexDigitValue(character).has_value();
			});
		}

		// The value of text digit `index` (0 is the most significant) of the 16-digit form of `value`.
		static uint64_t DigitAt(uint64_t value, size_t index)
		{
			return (value >> (4 * (UUID::TextLength - 1 - index))) & 0xf;
		}

	}

	std::string UUID::ToString() const
	{
		std::string text(TextLength, '0');
		for (size_t index = 0; index < TextLength; ++index)
			text[index] = Utils::HexDigits[Utils::DigitAt(m_Value, index)];
		return text;
	}

	bool UUID::MatchesPrefix(std::string_view prefix) const
	{
		if (!IsValidPrefix(prefix))
			return false;
		for (size_t index = 0; index < prefix.size(); ++index)
		{
			// IsValidPrefix accepted every character, so each has a value.
			if (*Utils::HexDigitValue(prefix[index]) != Utils::DigitAt(m_Value, index))
				return false;
		}
		return true;
	}

	std::optional<UUID> UUID::FromString(std::string_view text)
	{
		if (text.size() != TextLength)
			return std::nullopt;
		uint64_t value = 0;
		for (const char character : text)
		{
			const std::optional<uint64_t> digit = Utils::HexDigitValue(character);
			if (!digit)
				return std::nullopt;
			value = (value << 4) | *digit;
		}
		return UUID(value);
	}

	bool UUID::IsValidPrefix(std::string_view prefix)
	{
		return prefix.size() >= MinimumPrefixLength && prefix.size() <= TextLength && Utils::IsHexText(prefix);
	}

}
