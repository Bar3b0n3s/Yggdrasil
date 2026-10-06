#include "EnginePCH.h"
#include "Engine/Core/Json/JsonReader.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Utf8.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <deque>
#include <iterator>
#include <unordered_set>

// Parse runs in three steps, all without recursion per nesting level:
//   1. a structural pre-scan finds the bracket that would open level MaxJsonDepth + 1, so nlohmann never builds a deeper
//      tree (its parser is iterative, but a million nested arrays would still cost a node each);
//   2. nlohmann's non-throwing parse (json::parse(..., nullptr, false) + is_discarded()) checks the syntax and builds the
//      tree; a position-tracking input iterator locates its errors, which it reports without positions;
//   3. a scan of the now known-valid text finds what nlohmann accepts but the engine does not: duplicate keys (nlohmann
//      keeps the last value) and integer literals outside the 64-bit range (nlohmann turns them into floats).

namespace Engine {

	namespace {

		// An input iterator over the text that records how far nlohmann's lexer has read and whether it reached the end,
		// which locates a syntax error: the lexer stops on the byte that cannot continue the document.
		class TrackingIterator
		{
		public:
			using iterator_category = std::input_iterator_tag;
			using value_type = char;
			using difference_type = std::ptrdiff_t;
			using pointer = const char*;
			using reference = const char&;

			struct Progress
			{
				const char* Furthest = nullptr; // one past the last byte handed to the lexer
				bool ReachedEnd = false;        // the lexer asked for a byte past the end
			};

			TrackingIterator(const char* position, Progress* progress)
				: m_Position(position), m_Progress(progress)
			{
			}

			reference operator*() const { return *m_Position; }

			TrackingIterator& operator++()
			{
				++m_Position;
				m_Progress->Furthest = m_Position;
				return *this;
			}

			TrackingIterator operator++(int)
			{
				TrackingIterator previous = *this;
				++*this;
				return previous;
			}

			// nlohmann compares the read position with the end before every byte it reads.
			bool operator==(const TrackingIterator& other) const
			{
				const bool isEqual = m_Position == other.m_Position;
				if (isEqual)
					m_Progress->ReachedEnd = true;
				return isEqual;
			}
		private:
			const char* m_Position = nullptr;
			Progress* m_Progress = nullptr;
		};

		struct SourceLocation
		{
			uint32_t Line = 1;
			uint32_t Column = 1;
		};

		// A problem found by the scan of step 3.
		struct ScanProblem
		{
			size_t Offset = 0;
			std::string Message;
			std::string Pointer;
		};

	}

	namespace Utils {

		static constexpr std::string_view ByteOrderMark = "\xef\xbb\xbf";

		// The largest double magnitude that rounds to a finite float (FLT_MAX plus half an ulp is the tie that rounds up to
		// infinity).
		static constexpr double FloatRoundingLimit = 0x1.ffffffp+127;

		static bool IsJsonWhitespace(char character)
		{
			return character == ' ' || character == '\t' || character == '\n' || character == '\r';
		}

		// The start of the line that contains `offset`.
		static size_t LineStartOf(std::string_view text, size_t offset)
		{
			const size_t newline = text.substr(0, offset).rfind('\n');
			return newline == std::string_view::npos ? 0 : newline + 1;
		}

		// 1-based line and column (in bytes) of `offset`; the end of the text is a valid offset.
		static SourceLocation LocationOf(std::string_view text, size_t offset)
		{
			offset = std::min(offset, text.size());
			SourceLocation location;
			location.Line = static_cast<uint32_t>(std::ranges::count(text.substr(0, offset), '\n') + 1);
			location.Column = static_cast<uint32_t>(offset - LineStartOf(text, offset) + 1);
			return location;
		}

		static ErrorLocation PointerLocation(std::string pointer)
		{
			ErrorLocation location;
			location.JsonPointer = std::move(pointer);
			return location;
		}

		static Error ParseError(std::string_view text, size_t offset, std::string message)
		{
			const SourceLocation source = LocationOf(text, offset);
			ErrorLocation location;
			location.Line = source.Line;
			location.Column = source.Column;
			return Error(ErrorCode::Parse, std::move(message)).WithLocation(std::move(location));
		}

		static std::string EscapePointerToken(std::string_view key)
		{
			std::string escaped;
			escaped.reserve(key.size());
			for (const char character : key)
			{
				if (character == '~')
					escaped += "~0";
				else if (character == '/')
					escaped += "~1";
				else
					escaped += character;
			}
			return escaped;
		}

		// Step 1: the offset of the bracket that opens nesting level MaxJsonDepth + 1, if any. Strings are skipped so
		// brackets inside them do not count; for text that is valid up to that bracket the answer is exact.
		static std::optional<size_t> FindTooDeepBracket(std::string_view text)
		{
			size_t depth = 0;
			bool inString = false;
			for (size_t offset = 0; offset < text.size(); ++offset)
			{
				const char character = text[offset];
				if (inString)
				{
					if (character == '\\')
						++offset;
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
						if (++depth > MaxJsonDepth)
							return offset;
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
			return std::nullopt;
		}

		// Up to `maximum` bytes of the line before `offset`, without leading whitespace and starting on a code point, for
		// an error message; empty when that text is not printable UTF-8.
		static std::string ContextBefore(std::string_view text, size_t offset, size_t maximum)
		{
			const size_t start = std::max(LineStartOf(text, offset), offset > maximum ? offset - maximum : 0);
			std::string_view context = text.substr(start, offset - start);
			while (!context.empty() && (static_cast<uint8_t>(context.front()) & 0xc0) == 0x80)
				context.remove_prefix(1);
			while (!context.empty() && IsJsonWhitespace(context.front()))
				context.remove_prefix(1);
			const bool isPrintable = std::ranges::none_of(context, [](char character)
			{
				return static_cast<uint8_t>(character) < 0x20 && character != '\t';
			});
			if (!isPrintable || !IsValidUtf8(context))
				return {};
			return std::string(context);
		}

		// How far nlohmann reads `text` before it accepts or rejects it. json::accept stops on the first error (json::parse
		// reads one more token for its end-of-input check, which would hide the position).
		static TrackingIterator::Progress AcceptProgress(std::string_view text)
		{
			TrackingIterator::Progress progress{ .Furthest = text.data() };
			const TrackingIterator first(text.data(), &progress);
			const TrackingIterator last(text.data() + text.size(), &progress);
			const bool accepted = Json::accept(first, last, false);
			ENGINE_CORE_ASSERT(!accepted || progress.ReachedEnd, "An accepted JSON text is read to its end");
			return progress;
		}

		// Describes the syntax error nlohmann stopped at (it reports none itself in non-throwing mode).
		static Error DescribeSyntaxError(std::string_view text, const TrackingIterator::Progress& progress)
		{
			const size_t invalidUtf8 = FindInvalidUtf8(text);
			const size_t consumed = static_cast<size_t>(progress.Furthest - text.data());
			if (invalidUtf8 < consumed)
				return ParseError(text, invalidUtf8, "invalid UTF-8 byte sequence");
			if (progress.ReachedEnd || consumed == 0)
				return ParseError(text, text.size(), "unexpected end of the document");

			const size_t offset = consumed - 1;
			const auto byte = static_cast<uint8_t>(text[offset]);
			std::string what;
			if (byte == '\n' || byte == '\r')
				what = "line break";
			else if (byte < 0x20)
				what = std::format("control character U+{:04X} (control characters in strings must be escaped)", byte);
			else if (byte >= 0x80)
				what = "non-ASCII character outside a string";
			else
				what = std::format("'{}'", static_cast<char>(byte));

			const std::string context = ContextBefore(text, offset, 24);
			if (context.empty())
				return ParseError(text, offset, std::format("syntax error: unexpected {}", what));
			return ParseError(text, offset, std::format("syntax error: unexpected {} after '{}'", what, context));
		}

		static void AppendUtf8(std::string& text, uint32_t codePoint)
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

		// The value of hexadecimal digits that nlohmann already validated.
		static uint32_t HexValue(std::string_view digits)
		{
			uint32_t value = 0;
			for (const char digit : digits)
			{
				const auto lower = static_cast<char>(digit | 0x20);
				value = (value << 4) | static_cast<uint32_t>(digit <= '9' ? digit - '0' : lower - 'a' + 10);
			}
			return value;
		}

		// The value of the string literal whose escaped contents are `contents` (the text between the quotes). The text
		// was accepted by nlohmann, so every escape is well formed and surrogates come in pairs.
		static std::string UnescapeString(std::string_view contents)
		{
			std::string value;
			value.reserve(contents.size());
			for (size_t index = 0; index < contents.size(); ++index)
			{
				const char character = contents[index];
				if (character != '\\' || index + 1 >= contents.size())
				{
					value += character;
					continue;
				}
				const char escaped = contents[++index];
				switch (escaped)
				{
					case 'b': value += '\b'; break;
					case 'f': value += '\f'; break;
					case 'n': value += '\n'; break;
					case 'r': value += '\r'; break;
					case 't': value += '\t'; break;
					case 'u':
					{
						uint32_t codePoint = HexValue(contents.substr(index + 1, 4));
						index += 4;
						const bool isHighSurrogate = codePoint >= 0xd800 && codePoint <= 0xdbff;
						if (isHighSurrogate && index + 6 < contents.size() && contents.substr(index + 1, 2) == "\\u")
						{
							const uint32_t low = HexValue(contents.substr(index + 3, 4));
							codePoint = 0x10000 + ((codePoint - 0xd800) << 10) + (low - 0xdc00);
							index += 6;
						}
						AppendUtf8(value, codePoint);
						break;
					}
					default:
						value += escaped; // '"', '\\' and '/'
						break;
				}
			}
			return value;
		}

		// Step 3: walks text that nlohmann accepted and reports the first duplicate key or out-of-range integer literal,
		// with the JSON pointer of the offending member or value.
		class StructureScanner
		{
		public:
			explicit StructureScanner(std::string_view text)
				: m_Text(text)
			{
			}

			std::optional<ScanProblem> Run()
			{
				size_t offset = m_Text.starts_with(ByteOrderMark) ? ByteOrderMark.size() : 0;
				while (offset < m_Text.size())
				{
					const char character = m_Text[offset];
					switch (character)
					{
						case '{':
						case '[':
						{
							Frame& frame = m_Frames.emplace_back();
							frame.IsObject = character == '{';
							frame.ExpectingKey = frame.IsObject;
							++offset;
							break;
						}
						case '}':
						case ']':
							m_Frames.pop_back();
							CompleteValue();
							++offset;
							break;
						case ',':
							if (!m_Frames.empty() && m_Frames.back().IsObject)
								m_Frames.back().ExpectingKey = true;
							++offset;
							break;
						case ':':
							m_Frames.back().ExpectingKey = false;
							++offset;
							break;
						case '"':
						{
							const size_t end = StringEnd(offset);
							if (!m_Frames.empty() && m_Frames.back().IsObject && m_Frames.back().ExpectingKey)
							{
								if (std::optional<ScanProblem> problem = AddKey(offset, end))
									return problem;
							}
							else
							{
								CompleteValue();
							}
							offset = end;
							break;
						}
						default:
						{
							if (IsJsonWhitespace(character))
							{
								++offset;
								break;
							}
							const size_t end = TokenEnd(offset);
							if (std::optional<ScanProblem> problem = CheckNumber(offset, end))
								return problem;
							CompleteValue();
							offset = end;
							break;
						}
					}
				}
				return std::nullopt;
			}
		private:
			struct Frame
			{
				bool IsObject = false;
				bool ExpectingKey = false;
				size_t NextIndex = 0;   // arrays: the index of the element being read
				std::string CurrentKey; // objects: the key of the member being read
				std::unordered_set<std::string_view> Keys;
			};
		private:
			// One past the closing quote of the string starting at `offset`.
			size_t StringEnd(size_t offset) const
			{
				size_t index = offset + 1;
				while (index < m_Text.size() && m_Text[index] != '"')
					index += m_Text[index] == '\\' ? 2 : 1;
				return std::min(index + 1, m_Text.size());
			}

			size_t TokenEnd(size_t offset) const
			{
				size_t index = offset;
				while (index < m_Text.size() && !IsJsonWhitespace(m_Text[index]) && m_Text[index] != ',' && m_Text[index] != ']'
					&& m_Text[index] != '}')
				{
					++index;
				}
				return index;
			}

			void CompleteValue()
			{
				if (!m_Frames.empty() && !m_Frames.back().IsObject)
					++m_Frames.back().NextIndex;
			}

			std::string CurrentPointer() const
			{
				std::string pointer;
				for (const Frame& frame : m_Frames)
				{
					pointer += '/';
					pointer += frame.IsObject ? EscapePointerToken(frame.CurrentKey) : std::to_string(frame.NextIndex);
				}
				return pointer;
			}

			std::optional<ScanProblem> AddKey(size_t start, size_t end)
			{
				const std::string_view contents = m_Text.substr(start + 1, end - start - 2);
				std::string_view key = contents;
				if (contents.contains('\\'))
					key = m_UnescapedKeys.emplace_back(UnescapeString(contents));

				Frame& frame = m_Frames.back();
				frame.CurrentKey = key;
				if (!frame.Keys.insert(key).second)
				{
					return ScanProblem{ .Offset = start, .Message = std::format("duplicate key '{}'", key), .Pointer = CurrentPointer() };
				}
				return std::nullopt;
			}

			// nlohmann stores an integer literal that does not fit int64 or uint64 as a float; the engine rejects it, so
			// every number without fraction and exponent reads as an integer (JsonType::Integer).
			std::optional<ScanProblem> CheckNumber(size_t start, size_t end) const
			{
				const std::string_view token = m_Text.substr(start, end - start);
				const bool isNumber = !token.empty() && (token.front() == '-' || (token.front() >= '0' && token.front() <= '9'));
				if (!isNumber || token.find_first_of(".eE") != std::string_view::npos)
					return std::nullopt;

				std::from_chars_result result{};
				if (token.front() == '-')
				{
					int64_t value = 0;
					result = std::from_chars(token.data(), token.data() + token.size(), value);
				}
				else
				{
					uint64_t value = 0;
					result = std::from_chars(token.data(), token.data() + token.size(), value);
				}
				if (result.ec != std::errc::result_out_of_range)
					return std::nullopt;
				return ScanProblem{
					.Offset = start,
					.Message = std::format("integer {} is outside the 64-bit range", token),
					.Pointer = CurrentPointer(),
				};
			}
		private:
			std::string_view m_Text;
			std::vector<Frame> m_Frames;
			std::deque<std::string> m_UnescapedKeys; // owns the keys that contain escapes; stable addresses
		};

		// A number as text for an error message: integers exactly, floats in their shortest form.
		static std::string FormatNumber(const Json& value)
		{
			if (value.is_number_unsigned())
				return std::to_string(value.get<uint64_t>());
			if (value.is_number_integer())
				return std::to_string(value.get<int64_t>());
			return std::format("{}", value.get<double>());
		}

		static const Json* FindMemberValue(const Json& object, std::string_view key)
		{
			if (!object.is_object())
				return nullptr;
			for (const auto& [name, member] : object.get_ref<const Json::object_t&>())
			{
				if (name == key)
					return &member;
			}
			return nullptr;
		}

		template<typename T>
		static Result<T> ReadInteger(const JsonReader& reader, std::string_view typeName)
		{
			const Json& value = reader.GetValue();
			if (value.is_number_unsigned())
			{
				const auto number = value.get<uint64_t>();
				if (std::in_range<T>(number))
					return static_cast<T>(number);
			}
			else if (value.is_number_integer())
			{
				const auto number = value.get<int64_t>();
				if (std::in_range<T>(number))
					return static_cast<T>(number);
			}
			else if (value.is_number_float())
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("expected an integer without fraction or exponent, got {}", FormatNumber(value))));
			}
			else
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("expected number, got {}", JsonTypeToString(reader.GetType()))));
			}
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
				std::format("{} is out of range for {} ({} to {})", FormatNumber(value), typeName, std::numeric_limits<T>::min(),
					std::numeric_limits<T>::max())));
		}

	}

	std::string_view JsonTypeToString(JsonType type)
	{
		switch (type)
		{
			case JsonType::Null:    return "null";
			case JsonType::Bool:    return "boolean";
			case JsonType::Integer: return "number";
			case JsonType::Float:   return "number";
			case JsonType::String:  return "string";
			case JsonType::Array:   return "array";
			case JsonType::Object:  return "object";
		}

		ENGINE_CORE_ASSERT(false, "Unknown JsonType {}", std::to_underlying(type));
		return "unknown";
	}

	JsonType GetJsonType(const Json& value)
	{
		switch (value.type())
		{
			case Json::value_t::null:            return JsonType::Null;
			case Json::value_t::boolean:         return JsonType::Bool;
			case Json::value_t::number_integer:  return JsonType::Integer;
			case Json::value_t::number_unsigned: return JsonType::Integer;
			case Json::value_t::number_float:    return JsonType::Float;
			case Json::value_t::string:          return JsonType::String;
			case Json::value_t::array:           return JsonType::Array;
			case Json::value_t::object:          return JsonType::Object;
			case Json::value_t::binary:
			case Json::value_t::discarded:
				break;
		}

		ENGINE_CORE_ASSERT(false, "JSON value of type '{}' has no JsonType (binary and discarded values never come from Parse)", value.type_name());
		return JsonType::Null;
	}

	Result<Json> JsonReader::Parse(std::string_view text)
	{
		// Step 1. The text before the bracket tells whether an earlier syntax error comes first: it is a valid start of a
		// document exactly when nlohmann runs out of input on it.
		if (const std::optional<size_t> tooDeep = Utils::FindTooDeepBracket(text))
		{
			const TrackingIterator::Progress progress = Utils::AcceptProgress(text.substr(0, *tooDeep));
			if (!progress.ReachedEnd)
				return std::unexpected(Utils::DescribeSyntaxError(text, progress));
			return std::unexpected(
				Utils::ParseError(text, *tooDeep, std::format("arrays and objects are nested deeper than {} levels", MaxJsonDepth)));
		}

		// Step 2.
		Json document = Json::parse(text.begin(), text.end(), nullptr, false, false);
		if (document.is_discarded())
			return std::unexpected(Utils::DescribeSyntaxError(text, Utils::AcceptProgress(text)));

		// Step 3.
		Utils::StructureScanner scanner(text);
		if (std::optional<ScanProblem> problem = scanner.Run())
		{
			Error error = Utils::ParseError(text, problem->Offset, std::move(problem->Message));
			return std::unexpected(std::move(error).WithLocation(Utils::PointerLocation(std::move(problem->Pointer))));
		}
		return document;
	}

	JsonReader::JsonReader(const Json& value, std::string pointer)
		: m_Value(&value), m_Pointer(std::move(pointer))
	{
	}

	JsonType JsonReader::GetType() const
	{
		return GetJsonType(*m_Value);
	}

	bool JsonReader::IsNull() const
	{
		return m_Value->is_null();
	}

	bool JsonReader::IsObject() const
	{
		return m_Value->is_object();
	}

	bool JsonReader::IsArray() const
	{
		return m_Value->is_array();
	}

	Status JsonReader::ExpectType(JsonType expected) const
	{
		const JsonType actual = GetType();
		if (actual == expected || (expected == JsonType::Float && actual == JsonType::Integer))
			return {};
		if (expected == JsonType::Integer && actual == JsonType::Float)
		{
			return std::unexpected(MakeLocatedError(ErrorCode::Validation,
				std::format("expected an integer without fraction or exponent, got {}", Utils::FormatNumber(*m_Value))));
		}
		return std::unexpected(MakeLocatedError(ErrorCode::Validation,
			std::format("expected {}, got {}", JsonTypeToString(expected), JsonTypeToString(actual))));
	}

	Result<bool> JsonReader::ReadBool() const
	{
		ENGINE_TRY(ExpectType(JsonType::Bool));
		return m_Value->get<bool>();
	}

	Result<int32_t> JsonReader::ReadInt32() const
	{
		return Utils::ReadInteger<int32_t>(*this, "int32");
	}

	Result<uint32_t> JsonReader::ReadUInt32() const
	{
		return Utils::ReadInteger<uint32_t>(*this, "uint32");
	}

	Result<int64_t> JsonReader::ReadInt64() const
	{
		return Utils::ReadInteger<int64_t>(*this, "int64");
	}

	Result<uint64_t> JsonReader::ReadUInt64() const
	{
		return Utils::ReadInteger<uint64_t>(*this, "uint64");
	}

	Result<float> JsonReader::ReadFloat() const
	{
		ENGINE_TRY_ASSIGN(const double value, ReadDouble());
		if (!(std::fabs(value) < Utils::FloatRoundingLimit))
			return std::unexpected(MakeLocatedError(ErrorCode::Validation, std::format("{} is out of range for float", value)));
		return static_cast<float>(value);
	}

	Result<double> JsonReader::ReadDouble() const
	{
		ENGINE_TRY(ExpectType(JsonType::Float));
		const auto value = m_Value->get<double>();
		if (!std::isfinite(value))
			return std::unexpected(MakeLocatedError(ErrorCode::Validation, "expected a finite number"));
		return value;
	}

	Result<std::string> JsonReader::ReadString() const
	{
		ENGINE_TRY(ExpectType(JsonType::String));
		return m_Value->get_ref<const std::string&>();
	}

	Result<UUID> JsonReader::ReadUUID() const
	{
		if (!m_Value->is_string())
		{
			return std::unexpected(MakeLocatedError(ErrorCode::Validation,
				std::format("expected a 16-digit hex UUID string, got {}", JsonTypeToString(GetType()))));
		}
		const std::optional<UUID> uuid = UUID::FromString(m_Value->get_ref<const std::string&>());
		if (!uuid)
			return std::unexpected(MakeLocatedError(ErrorCode::Validation, "expected a 16-digit hex UUID string"));
		return *uuid;
	}

	bool JsonReader::HasMember(std::string_view key) const
	{
		return Utils::FindMemberValue(*m_Value, key) != nullptr;
	}

	Result<JsonReader> JsonReader::GetMember(std::string_view key) const
	{
		ENGINE_TRY(ExpectType(JsonType::Object));
		const Json* member = Utils::FindMemberValue(*m_Value, key);
		if (member == nullptr)
			return std::unexpected(MakeLocatedError(ErrorCode::Validation, std::format("missing required field '{}'", key)));
		return JsonReader(*member, AppendPointer(m_Pointer, key));
	}

	std::optional<JsonReader> JsonReader::FindMember(std::string_view key) const
	{
		const Json* member = Utils::FindMemberValue(*m_Value, key);
		if (member == nullptr)
			return std::nullopt;
		return JsonReader(*member, AppendPointer(m_Pointer, key));
	}

	Result<std::vector<std::string>> JsonReader::GetMemberNames() const
	{
		ENGINE_TRY(ExpectType(JsonType::Object));
		std::vector<std::string> names;
		names.reserve(m_Value->size());
		for (const auto& [name, member] : m_Value->get_ref<const Json::object_t&>())
			names.push_back(name);
		return names;
	}

	Result<std::vector<std::string>> JsonReader::FindUnknownMembers(std::span<const std::string_view> knownKeys) const
	{
		ENGINE_TRY(ExpectType(JsonType::Object));
		std::vector<std::string> unknown;
		for (const auto& [name, member] : m_Value->get_ref<const Json::object_t&>())
		{
			if (std::ranges::find(knownKeys, std::string_view(name)) == knownKeys.end())
				unknown.push_back(name);
		}
		return unknown;
	}

	Result<size_t> JsonReader::GetArraySize() const
	{
		ENGINE_TRY(ExpectType(JsonType::Array));
		return m_Value->size();
	}

	Result<JsonReader> JsonReader::GetElement(size_t index) const
	{
		ENGINE_TRY(ExpectType(JsonType::Array));
		if (index >= m_Value->size())
		{
			return std::unexpected(MakeLocatedError(ErrorCode::Validation,
				std::format("index {} is out of range for an array of {} elements", index, m_Value->size())));
		}
		return JsonReader((*m_Value)[index], AppendPointer(m_Pointer, index));
	}

	Result<uint32_t> JsonReader::ReadFormatHeader(std::string_view expectedFormat, uint32_t minimumVersion, uint32_t supportedVersion) const
	{
		ENGINE_CORE_ASSERT(minimumVersion <= supportedVersion, "ReadFormatHeader: minimum version {} is above the supported version {}",
			minimumVersion, supportedVersion);
		ENGINE_TRY(ExpectType(JsonType::Object));

		ENGINE_TRY_ASSIGN(const JsonReader format, GetMember("Format"));
		ENGINE_TRY_ASSIGN(const std::string formatName, format.ReadString());
		if (formatName != expectedFormat)
		{
			return std::unexpected(format.MakeLocatedError(ErrorCode::Validation,
				std::format("expected a '{}' file, got Format '{}'", expectedFormat, formatName)));
		}

		ENGINE_TRY_ASSIGN(const JsonReader version, GetMember("Version"));
		ENGINE_TRY_ASSIGN(const uint64_t fileVersion, version.ReadUInt64());
		if (fileVersion > supportedVersion)
		{
			return std::unexpected(version.MakeLocatedError(ErrorCode::UnsupportedVersion,
				std::format("'{}' version {} is newer than this build supports (version {})", expectedFormat, fileVersion, supportedVersion)));
		}
		if (fileVersion < minimumVersion)
		{
			return std::unexpected(version.MakeLocatedError(ErrorCode::Validation,
				std::format("'{}' version {} is older than the oldest readable version {}", expectedFormat, fileVersion, minimumVersion)));
		}
		return static_cast<uint32_t>(fileVersion);
	}

	Error JsonReader::MakeLocatedError(ErrorCode code, std::string message) const
	{
		return Error(code, std::move(message)).WithLocation(Utils::PointerLocation(m_Pointer));
	}

	std::string JsonReader::AppendPointer(std::string_view pointer, std::string_view key)
	{
		std::string result;
		result.reserve(pointer.size() + 1 + key.size());
		result += pointer;
		result += '/';
		result += Utils::EscapePointerToken(key);
		return result;
	}

	std::string JsonReader::AppendPointer(std::string_view pointer, size_t index)
	{
		return std::format("{}/{}", pointer, index);
	}

}
