#include "EnginePCH.h"
#include "Engine/Core/Utf8.h"

namespace Engine {

	namespace Utils {

		// The length of the well-formed sequence that starts at `offset`, or 0 when it is ill-formed (RFC 3629 table 3-7:
		// the second byte's range depends on the lead byte, which excludes overlong forms, surrogates and code points
		// above U+10FFFF).
		static size_t SequenceLength(std::string_view text, size_t offset)
		{
			const auto byteAt = [text](size_t index)
			{
				return static_cast<uint8_t>(text[index]);
			};
			const auto isContinuation = [](uint8_t value)
			{
				return value >= 0x80 && value <= 0xbf;
			};

			const uint8_t lead = byteAt(offset);
			if (lead < 0x80)
				return 1;

			size_t length = 0;
			uint8_t secondMin = 0x80;
			uint8_t secondMax = 0xbf;
			if (lead >= 0xc2 && lead <= 0xdf)
			{
				length = 2;
			}
			else if (lead >= 0xe0 && lead <= 0xef)
			{
				length = 3;
				if (lead == 0xe0)
					secondMin = 0xa0; // overlong below U+0800
				else if (lead == 0xed)
					secondMax = 0x9f; // surrogates U+D800-U+DFFF
			}
			else if (lead >= 0xf0 && lead <= 0xf4)
			{
				length = 4;
				if (lead == 0xf0)
					secondMin = 0x90; // overlong below U+10000
				else if (lead == 0xf4)
					secondMax = 0x8f; // above U+10FFFF
			}
			else
			{
				return 0; // continuation byte, overlong C0/C1 lead, or F5-FF
			}

			if (text.size() - offset < length)
				return 0;
			const uint8_t second = byteAt(offset + 1);
			if (second < secondMin || second > secondMax)
				return 0;
			for (size_t index = 2; index < length; ++index)
			{
				if (!isContinuation(byteAt(offset + index)))
					return 0;
			}
			return length;
		}

	}

	bool IsValidUtf8(std::string_view text)
	{
		return FindInvalidUtf8(text) == text.size();
	}

	size_t FindInvalidUtf8(std::string_view text)
	{
		size_t offset = 0;
		while (offset < text.size())
		{
			const size_t length = Utils::SequenceLength(text, offset);
			if (length == 0)
				return offset;
			offset += length;
		}
		return text.size();
	}

}
