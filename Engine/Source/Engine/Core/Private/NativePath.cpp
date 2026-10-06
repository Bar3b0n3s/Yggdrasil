#include "EnginePCH.h"
#include "Engine/Core/Private/NativePath.h"

#include "Engine/Core/Utf8.h"

namespace Engine {

	namespace Utils {

		using NativeChar = std::filesystem::path::value_type;

		static constexpr char32_t ReplacementCharacter = 0xfffd;

		static size_t LeadByteLength(uint8_t lead)
		{
			if (lead < 0x80)
				return 1;
			if (lead < 0xe0)
				return 2;
			if (lead < 0xf0)
				return 3;
			return 4;
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
				const size_t length = LeadByteLength(lead);
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

		// UTF-8 in the host's path encoding: UTF-8 bytes on POSIX, UTF-16 on Windows.
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
				static_assert(sizeof(Char) == 2, "wide host paths are UTF-16");
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
						const char32_t offset = codePoint - 0x10000;
						wide += static_cast<Char>(0xd800 + (offset >> 10));
						wide += static_cast<Char>(0xdc00 + (offset & 0x3ff));
					}
				}
				return wide;
			}
		}

		// A host path string as UTF-8 (see ToNative).
		template<typename Char>
		static std::string FromNative(std::basic_string_view<Char> native)
		{
			if constexpr (sizeof(Char) == 1)
			{
				return std::string(native.begin(), native.end());
			}
			else
			{
				static_assert(sizeof(Char) == 2, "wide host paths are UTF-16");
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

		template<typename Char>
		static std::basic_string<Char> GenericNative(const std::filesystem::path& path)
		{
			if constexpr (std::is_same_v<Char, wchar_t>)
				return path.generic_wstring();
			else
				return path.generic_string();
		}

		std::filesystem::path PathFromUtf8(std::string_view utf8)
		{
			return std::filesystem::path(ToNative<NativeChar>(utf8));
		}

		std::string PathToUtf8(const std::filesystem::path& path)
		{
			const std::basic_string<NativeChar> generic = GenericNative<NativeChar>(path);
			return FromNative<NativeChar>(generic);
		}

		std::string FileNameToUtf8(const std::filesystem::path& path)
		{
			const std::filesystem::path fileName = path.filename();
			return FromNative<NativeChar>(fileName.native());
		}

	}

}
