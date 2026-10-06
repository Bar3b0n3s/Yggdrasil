#include "EnginePCH.h"
#include "Engine/Platform/Private/PathsUtf8.h"

#include "Engine/Core/Utf8.h"

namespace Engine {

	namespace Utils {

		using NativeChar = std::filesystem::path::value_type;

		static constexpr char32_t ReplacementCharacter = 0xfffd;

		// The length of the sequence that `lead` starts, assuming it is well-formed.
		static size_t Utf8SequenceLength(uint8_t lead)
		{
			if (lead < 0x80)
				return 1;
			if (lead < 0xe0)
				return 2;
			if (lead < 0xf0)
				return 3;
			return 4;
		}

		static void AppendUtf8(std::string& text, char32_t codePoint)
		{
			if (codePoint < 0x80)
			{
				text += static_cast<char>(codePoint);
			}
			else if (codePoint < 0x800)
			{
				text += static_cast<char>(0xc0 | (codePoint >> 6));
				text += static_cast<char>(0x80 | (codePoint & 0x3f));
			}
			else if (codePoint < 0x10000)
			{
				text += static_cast<char>(0xe0 | (codePoint >> 12));
				text += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
				text += static_cast<char>(0x80 | (codePoint & 0x3f));
			}
			else
			{
				text += static_cast<char>(0xf0 | (codePoint >> 18));
				text += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f));
				text += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f));
				text += static_cast<char>(0x80 | (codePoint & 0x3f));
			}
		}

		// The code points of `utf8`; each byte that does not start a well-formed sequence becomes U+FFFD.
		static std::u32string DecodeUtf8(std::string_view utf8)
		{
			std::u32string codePoints;
			codePoints.reserve(utf8.size());
			size_t offset = 0;
			while (offset < utf8.size())
			{
				const auto lead = static_cast<uint8_t>(utf8[offset]);
				const size_t length = Utf8SequenceLength(lead);
				const std::string_view window = utf8.substr(offset, std::min<size_t>(4, utf8.size() - offset));
				if (FindInvalidUtf8(window) < length)
				{
					codePoints.push_back(ReplacementCharacter);
					++offset;
					continue;
				}

				char32_t codePoint = length == 1 ? lead : static_cast<char32_t>(lead & (0x7fu >> length));
				for (size_t index = 1; index < length; ++index)
					codePoint = (codePoint << 6) | (static_cast<uint8_t>(utf8[offset + index]) & 0x3fu);
				codePoints.push_back(codePoint);
				offset += length;
			}
			return codePoints;
		}

		// UTF-8 in the encoding of `Char`, ill-formed sequences replaced. A template, so that only the branch of the host's
		// character type is compiled.
		template<typename Char>
		static std::basic_string<Char> ToNative(std::string_view utf8)
		{
			if constexpr (sizeof(Char) == 1)
			{
				if (IsValidUtf8(utf8))
					return std::basic_string<Char>(utf8.begin(), utf8.end());
				std::string sanitized;
				sanitized.reserve(utf8.size());
				for (const char32_t codePoint : DecodeUtf8(utf8))
					AppendUtf8(sanitized, codePoint);
				return std::basic_string<Char>(sanitized.begin(), sanitized.end());
			}
			else
			{
				static_assert(sizeof(Char) == 2, "wide host strings are UTF-16");
				std::basic_string<Char> wide;
				wide.reserve(utf8.size());
				for (const char32_t codePoint : DecodeUtf8(utf8))
				{
					if (codePoint < 0x10000)
					{
						wide += static_cast<Char>(codePoint);
					}
					else
					{
						const char32_t surrogateOffset = codePoint - 0x10000;
						wide += static_cast<Char>(0xd800 + (surrogateOffset >> 10));
						wide += static_cast<Char>(0xdc00 + (surrogateOffset & 0x3ff));
					}
				}
				return wide;
			}
		}

		// A string in the encoding of `Char` as UTF-8 (see ToNative): bytes unchanged, UTF-16 with unpaired surrogates
		// replaced.
		template<typename Char>
		static std::string FromNative(std::basic_string_view<Char> native)
		{
			if constexpr (sizeof(Char) == 1)
			{
				return std::string(native.begin(), native.end());
			}
			else
			{
				static_assert(sizeof(Char) == 2, "wide host strings are UTF-16");
				std::string utf8;
				utf8.reserve(native.size());
				for (size_t index = 0; index < native.size(); ++index)
				{
					const auto unit = static_cast<char32_t>(native[index]);
					const bool isHigh = unit >= 0xd800 && unit <= 0xdbff;
					const bool isLow = unit >= 0xdc00 && unit <= 0xdfff;
					if (isHigh && index + 1 < native.size())
					{
						const auto next = static_cast<char32_t>(native[index + 1]);
						if (next >= 0xdc00 && next <= 0xdfff)
						{
							AppendUtf8(utf8, 0x10000 + ((unit - 0xd800) << 10) + (next - 0xdc00));
							++index;
							continue;
						}
					}
					AppendUtf8(utf8, isHigh || isLow ? ReplacementCharacter : unit);
				}
				return utf8;
			}
		}

		NativeString NativeStringFromUtf8(std::string_view utf8)
		{
			return ToNative<NativeChar>(utf8);
		}

		std::string NativeStringToUtf8(NativeStringView native)
		{
			return FromNative<NativeChar>(native);
		}

		std::filesystem::path NativePathFromUtf8(std::string_view utf8)
		{
			return std::filesystem::path(NativeStringFromUtf8(utf8));
		}

		std::string NativePathToUtf8(const std::filesystem::path& path)
		{
			return NativeStringToUtf8(path.native());
		}

	}

}
