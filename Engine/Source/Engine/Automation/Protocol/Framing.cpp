#include "EnginePCH.h"
#include "Engine/Automation/Protocol/Framing.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"

#include <algorithm>
#include <format>

// The decoder re-reads the header of the frame at the front of its buffer on every Next call: a header is at most
// MaxFrameHeaderBytes, so the cost is bounded, and the decoder needs no state beyond its bytes and its failure. A frame that
// has been returned is erased from the buffer; the server receives in chunks of a few KB, so the erase moves little.

namespace Engine {

	namespace Utils {

		constexpr std::string_view ContentLengthName = "Content-Length:";
		constexpr std::string_view ContentTypeName = "Content-Type:";
		// More digits than this cannot be at most MaxFramePayloadBytes (64 MB has 8 digits); the value is reported as Oversized.
		constexpr size_t MaxContentLengthDigits = 12;
		static_assert(MaxFramePayloadBytes < 1'000'000'000'000ull, "MaxContentLengthDigits must cover MaxFramePayloadBytes");

		[[nodiscard]] static char ToLowerAscii(char character)
		{
			return character >= 'A' && character <= 'Z' ? static_cast<char>(character - 'A' + 'a') : character;
		}

		// True when the first `count` characters of `text` equal those of `name`, ignoring ASCII case.
		[[nodiscard]] static bool StartsWithIgnoreCase(std::string_view text, std::string_view name, size_t count)
		{
			for (size_t index = 0; index < count; ++index)
			{
				if (ToLowerAscii(text[index]) != ToLowerAscii(name[index]))
					return false;
			}
			return true;
		}

		[[nodiscard]] static bool IsHeaderWhitespace(char character)
		{
			return character == ' ' || character == '\t';
		}

		// The value of a header line after its name: optional whitespace around it.
		[[nodiscard]] static std::string_view TrimHeaderValue(std::string_view value)
		{
			while (!value.empty() && IsHeaderWhitespace(value.front()))
				value.remove_prefix(1);
			while (!value.empty() && IsHeaderWhitespace(value.back()))
				value.remove_suffix(1);
			return value;
		}

		// The result of reading a header section.
		struct HeaderScan
		{
			std::optional<FrameErrorKind> Failure{};
			std::string Detail{};
			bool Complete = false;    // the blank line ending the section arrived
			size_t HeaderSize = 0;    // the section's size, blank line included, when complete
			size_t PayloadLength = 0; // the Content-Length, when the first line is complete
		};

		// Reads the Content-Length line `line` (without its CR LF) into `scan`; a length over `maxPayloadBytes` is Oversized.
		static void ReadContentLength(std::string_view line, size_t maxPayloadBytes, HeaderScan& scan)
		{
			const std::string_view value = TrimHeaderValue(line.substr(ContentLengthName.size()));
			if (value.empty())
			{
				scan.Failure = FrameErrorKind::MalformedHeader;
				scan.Detail = "Content-Length has no value";
				return;
			}
			for (const char character : value)
			{
				if (character < '0' || character > '9')
				{
					scan.Failure = FrameErrorKind::MalformedHeader;
					scan.Detail = "Content-Length is not a decimal number";
					return;
				}
			}
			if (value.size() > MaxContentLengthDigits)
			{
				scan.Failure = FrameErrorKind::Oversized;
				scan.Detail = std::format("Content-Length has {} digits; at most {} bytes are accepted", value.size(), maxPayloadBytes);
				return;
			}
			uint64_t length = 0;
			for (const char character : value)
				length = length * 10 + static_cast<uint64_t>(character - '0');
			if (length > maxPayloadBytes)
			{
				scan.Failure = FrameErrorKind::Oversized;
				scan.Detail = std::format("Content-Length {} is over the limit of {} bytes", length, maxPayloadBytes);
				return;
			}
			scan.PayloadLength = static_cast<size_t>(length);
		}

		// Reads the header section at the front of `bytes`, as far as it has arrived. Fails as soon as the bytes can no longer
		// be the start of a valid header: a first line that cannot start with "Content-Length:", a bare CR or LF, an
		// unexpected header line, an oversized Content-Length (as soon as its line is complete) or a section over
		// MaxFrameHeaderBytes.
		[[nodiscard]] static HeaderScan ScanHeader(std::string_view bytes, size_t maxPayloadBytes)
		{
			HeaderScan scan;
			const size_t prefix = std::min(bytes.size(), ContentLengthName.size());
			if (!StartsWithIgnoreCase(bytes, ContentLengthName, prefix))
			{
				scan.Failure = FrameErrorKind::NotContentLength;
				scan.Detail = "the stream does not start with a Content-Length header";
				return scan;
			}

			const size_t limit = std::min(bytes.size(), MaxFrameHeaderBytes);
			size_t lineStart = 0;
			size_t lineNumber = 0;
			for (size_t index = 0; index < limit; ++index)
			{
				const char character = bytes[index];
				if (character == '\n')
				{
					scan.Failure = FrameErrorKind::MalformedHeader;
					scan.Detail = "a header line ends with a bare LF instead of CR LF";
					return scan;
				}
				if (character != '\r')
					continue;
				if (index + 1 >= bytes.size())
					return scan; // the LF has not arrived yet
				if (index + 2 > MaxFrameHeaderBytes)
					break; // the section cannot end within the limit
				if (bytes[index + 1] != '\n')
				{
					scan.Failure = FrameErrorKind::MalformedHeader;
					scan.Detail = "a header line contains a bare CR";
					return scan;
				}

				const std::string_view line = bytes.substr(lineStart, index - lineStart);
				if (line.empty())
				{
					if (lineNumber == 0)
					{
						scan.Failure = FrameErrorKind::MalformedHeader;
						scan.Detail = "the header section has no Content-Length";
						return scan;
					}
					scan.Complete = true;
					scan.HeaderSize = index + 2;
					return scan;
				}
				if (lineNumber == 0)
				{
					ReadContentLength(line, maxPayloadBytes, scan);
					if (scan.Failure.has_value())
						return scan;
				}
				else if (line.size() >= ContentLengthName.size() && StartsWithIgnoreCase(line, ContentLengthName, ContentLengthName.size()))
				{
					scan.Failure = FrameErrorKind::MalformedHeader;
					scan.Detail = "Content-Length is repeated";
					return scan;
				}
				else if (line.size() < ContentTypeName.size() || !StartsWithIgnoreCase(line, ContentTypeName, ContentTypeName.size()))
				{
					scan.Failure = FrameErrorKind::MalformedHeader;
					scan.Detail = "only Content-Length and Content-Type headers are accepted";
					return scan;
				}
				++lineNumber;
				lineStart = index + 2;
				++index; // skip the LF
			}

			if (bytes.size() >= MaxFrameHeaderBytes)
			{
				scan.Failure = FrameErrorKind::MalformedHeader;
				scan.Detail = std::format("the header section is longer than {} bytes", MaxFrameHeaderBytes);
			}
			return scan;
		}

		// The nesting depth of arrays and objects in `payload`, counted outside strings, stopping as soon as it passes
		// MaxMessageNestingDepth. The payload need not be valid JSON: unbalanced closers never take the depth below zero.
		[[nodiscard]] static bool IsNestedTooDeep(std::string_view payload)
		{
			size_t depth = 0;
			bool inString = false;
			bool escaped = false;
			for (const char character : payload)
			{
				if (inString)
				{
					if (escaped)
						escaped = false;
					else if (character == '\\')
						escaped = true;
					else if (character == '"')
						inString = false;
					continue;
				}
				switch (character)
				{
					case '"':
						inString = true;
						break;
					case '[':
					case '{':
						if (++depth > MaxMessageNestingDepth)
							return true;
						break;
					case ']':
					case '}':
						if (depth > 0)
							--depth;
						break;
					default:
						break;
				}
			}
			return false;
		}

	}

	std::string_view FrameErrorKindToString(FrameErrorKind kind)
	{
		switch (kind)
		{
			case FrameErrorKind::NotContentLength: return "NotContentLength";
			case FrameErrorKind::MalformedHeader:  return "MalformedHeader";
			case FrameErrorKind::Oversized:        return "Oversized";
			case FrameErrorKind::InvalidUtf8:      return "InvalidUtf8";
			case FrameErrorKind::TooDeep:          return "TooDeep";
		}
		return "Unknown";
	}

	FrameDecoder::FrameDecoder(size_t maxPayloadBytes)
		: m_MaxPayloadBytes(std::min(maxPayloadBytes, MaxFramePayloadBytes))
	{
	}

	void FrameDecoder::SetMaxPayloadBytes(size_t maxPayloadBytes)
	{
		m_MaxPayloadBytes = std::min(maxPayloadBytes, MaxFramePayloadBytes);
	}

	void FrameDecoder::Append(std::span<const std::byte> bytes)
	{
		if (m_Failure.has_value())
			return;
		m_Buffer.insert(m_Buffer.end(), bytes.begin(), bytes.end());
	}

	Result<std::optional<std::string>> FrameDecoder::Next()
	{
		if (m_Failure.has_value())
			return MakeError(ErrorCode::Validation, "invalid frame ({}): the connection must be closed", FrameErrorKindToString(*m_Failure));
		if (m_Buffer.empty())
			return std::optional<std::string>();

		const std::string_view bytes(reinterpret_cast<const char*>(m_Buffer.data()), m_Buffer.size());
		const Utils::HeaderScan scan = Utils::ScanHeader(bytes, m_MaxPayloadBytes);
		const auto fail = [this](FrameErrorKind kind, std::string_view detail) -> Result<std::optional<std::string>>
		{
			m_Failure = kind;
			m_Buffer.clear();
			m_Buffer.shrink_to_fit();
			return MakeError(ErrorCode::Validation, "invalid frame ({}): {}", FrameErrorKindToString(kind), detail);
		};
		if (scan.Failure.has_value())
			return fail(*scan.Failure, scan.Detail);
		if (!scan.Complete || bytes.size() - scan.HeaderSize < scan.PayloadLength)
			return std::optional<std::string>();

		std::string payload(bytes.substr(scan.HeaderSize, scan.PayloadLength));
		if (!IsValidUtf8(payload))
			return fail(FrameErrorKind::InvalidUtf8, std::format("the payload is not valid UTF-8 (byte {})", FindInvalidUtf8(payload)));
		if (Utils::IsNestedTooDeep(payload))
			return fail(FrameErrorKind::TooDeep, std::format("the payload nests arrays and objects deeper than {}", MaxMessageNestingDepth));

		m_Buffer.erase(m_Buffer.begin(), m_Buffer.begin() + static_cast<std::ptrdiff_t>(scan.HeaderSize + scan.PayloadLength));
		return std::optional<std::string>(std::move(payload));
	}

	std::string EncodeFrame(std::string_view payload)
	{
		ENGINE_CORE_ASSERT(payload.size() <= MaxFramePayloadBytes, "A frame payload of {} bytes is over the limit of {} bytes",
			payload.size(), MaxFramePayloadBytes);
		std::string frame = std::format("Content-Length: {}\r\n\r\n", payload.size());
		frame.append(payload);
		return frame;
	}

}
