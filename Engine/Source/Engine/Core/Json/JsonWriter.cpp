#include "EnginePCH.h"
#include "Engine/Core/Json/JsonWriter.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"

#include <nlohmann/json.hpp>

#include <charconv>
#include <cmath>
#include <unordered_set>

namespace Engine {

	namespace Utils {

		// The largest double magnitude that rounds to a finite float (FLT_MAX plus half an ulp rounds up to infinity).
		static constexpr double FloatRoundingLimit = 0x1.ffffffp+127;

		// `text` with every ill-formed UTF-8 sequence replaced by U+FFFD, so an error message about it stays valid UTF-8.
		static std::string SanitizedUtf8(std::string_view text)
		{
			std::string sanitized;
			sanitized.reserve(text.size());
			while (!text.empty())
			{
				const size_t invalid = FindInvalidUtf8(text);
				sanitized += text.substr(0, invalid);
				if (invalid == text.size())
					break;
				sanitized += "\xef\xbf\xbd";
				text.remove_prefix(invalid + 1);
			}
			return sanitized;
		}

		// A JSON string literal: '"' and '\' escaped, \b \f \n \r \t for those controls, \u00xx for the other code points
		// below U+0020, everything else (valid UTF-8) as is.
		static std::string QuoteString(std::string_view text)
		{
			static constexpr std::string_view HexDigits = "0123456789abcdef";
			std::string quoted;
			quoted.reserve(text.size() + 2);
			quoted += '"';
			for (const char character : text)
			{
				switch (character)
				{
					case '"':  quoted += "\\\""; break;
					case '\\': quoted += "\\\\"; break;
					case '\b': quoted += "\\b"; break;
					case '\f': quoted += "\\f"; break;
					case '\n': quoted += "\\n"; break;
					case '\r': quoted += "\\r"; break;
					case '\t': quoted += "\\t"; break;
					default:
					{
						const auto value = static_cast<uint8_t>(character);
						if (value < 0x20)
						{
							quoted += "\\u00";
							quoted += HexDigits[value >> 4];
							quoted += HexDigits[value & 0x0f];
						}
						else
						{
							quoted += character;
						}
						break;
					}
				}
			}
			quoted += '"';
			return quoted;
		}

		template<typename T>
		static std::string NumberText(T value)
		{
			std::array<char, 32> buffer{};
			const std::to_chars_result result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value);
			ENGINE_CORE_VERIFY(result.ec == std::errc(), "std::to_chars needs more than {} characters", buffer.size());
			return std::string(buffer.data(), result.ptr);
		}

	}

	struct JsonWriter::State
	{
		struct Container
		{
			bool IsObject = false;
			std::string Pointer;                   // the JSON pointer of the container itself
			std::string Text;                      // objects: the rendered members so far
			size_t MemberCount = 0;                // objects
			std::optional<std::string> PendingKey; // objects: the key written for the next value
			std::unordered_set<std::string> Keys;  // objects: every key written, for duplicate detection
			std::vector<std::string> Elements;     // arrays: the rendered elements so far
			bool HasContainerElement = false;      // arrays: an element is an object or an array (expanded layout)
		};

		JsonStyle Style = JsonStyle::Pretty;
		std::vector<Container> Stack;
		std::string Output;
		bool HasRoot = false;
		bool IsFinished = false;
		std::optional<Error> FirstError;

		bool IsPretty() const { return Style == JsonStyle::Pretty; }

		static std::string Indent(size_t level) { return std::string(level, '\t'); }

		// False (after asserting) when a value cannot be written here: after Finish, a second root value, or inside an
		// object without a key.
		bool CanWriteValue() const
		{
			ENGINE_CORE_ASSERT(!IsFinished, "JsonWriter: a value was written after Finish");
			if (IsFinished)
				return false;
			if (Stack.empty())
			{
				ENGINE_CORE_ASSERT(!HasRoot, "JsonWriter: a document has exactly one root value");
				return !HasRoot;
			}
			const Container& top = Stack.back();
			ENGINE_CORE_ASSERT(!top.IsObject || top.PendingKey.has_value(), "JsonWriter: a value inside an object needs a key (WriteKey)");
			return !top.IsObject || top.PendingKey.has_value();
		}

		// The JSON pointer of the value about to be written.
		std::string ValuePointer() const
		{
			if (Stack.empty())
				return {};
			const Container& top = Stack.back();
			if (top.IsObject)
				return top.PendingKey ? JsonReader::AppendPointer(top.Pointer, Utils::SanitizedUtf8(*top.PendingKey)) : top.Pointer;
			return JsonReader::AppendPointer(top.Pointer, top.Elements.size());
		}

		void RecordError(std::string message, std::string pointer)
		{
			if (FirstError)
				return;
			ErrorLocation location;
			location.JsonPointer = std::move(pointer);
			FirstError = Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location));
		}

		// Places a rendered value (a scalar or a finished container) into the open container or as the root.
		void AddValue(std::string text, bool isContainer)
		{
			if (Stack.empty())
			{
				Output = std::move(text);
				HasRoot = true;
				return;
			}

			Container& top = Stack.back();
			if (!top.IsObject)
			{
				top.Elements.push_back(std::move(text));
				top.HasContainerElement = top.HasContainerElement || isContainer;
				return;
			}

			if (top.MemberCount > 0)
				top.Text += ',';
			if (IsPretty())
			{
				top.Text += '\n';
				top.Text += Indent(Stack.size());
				top.Text += Utils::QuoteString(Utils::SanitizedUtf8(*top.PendingKey));
				top.Text += ": ";
			}
			else
			{
				top.Text += Utils::QuoteString(Utils::SanitizedUtf8(*top.PendingKey));
				top.Text += ':';
			}
			top.Text += text;
			++top.MemberCount;
			top.PendingKey.reset();
		}

		void AddScalar(std::string text)
		{
			if (CanWriteValue())
				AddValue(std::move(text), false);
		}

		void BeginContainer(bool isObject)
		{
			if (!CanWriteValue())
				return;
			Container container;
			container.IsObject = isObject;
			container.Pointer = ValuePointer();
			Stack.push_back(std::move(container));
		}

		void EndContainer(bool isObject)
		{
			ENGINE_CORE_ASSERT(!IsFinished, "JsonWriter: End{} after Finish", isObject ? "Object" : "Array");
			const bool isOpen = !IsFinished && !Stack.empty() && Stack.back().IsObject == isObject;
			ENGINE_CORE_ASSERT(isOpen, "JsonWriter: End{} without a matching Begin{}", isObject ? "Object" : "Array", isObject ? "Object" : "Array");
			if (!isOpen)
				return;
			ENGINE_CORE_ASSERT(!Stack.back().PendingKey.has_value(), "JsonWriter: EndObject after a key without a value");

			const size_t level = Stack.size();
			Container container = std::move(Stack.back());
			Stack.pop_back();

			std::string text;
			if (isObject)
			{
				if (container.MemberCount == 0)
					text = "{}";
				else if (IsPretty())
					text = "{" + container.Text + "\n" + Indent(level - 1) + "}";
				else
					text = "{" + container.Text + "}";
			}
			else if (container.Elements.empty())
			{
				text = "[]";
			}
			else
			{
				const bool isExpanded = IsPretty() && container.HasContainerElement;
				const std::string_view separator = isExpanded ? ",\n" : (IsPretty() ? ", " : ",");
				text = isExpanded ? "[\n" : "[";
				for (size_t index = 0; index < container.Elements.size(); ++index)
				{
					if (index > 0)
						text += separator;
					if (isExpanded)
						text += Indent(level);
					text += container.Elements[index];
				}
				text += isExpanded ? "\n" + Indent(level - 1) + "]" : "]";
			}
			AddValue(std::move(text), true);
		}
	};

	JsonWriter::JsonWriter(JsonStyle style)
		: m_State(CreateScope<State>())
	{
		m_State->Style = style;
	}

	JsonWriter::~JsonWriter() = default;

	void JsonWriter::BeginObject()
	{
		m_State->BeginContainer(true);
	}

	void JsonWriter::EndObject()
	{
		m_State->EndContainer(true);
	}

	void JsonWriter::BeginArray()
	{
		m_State->BeginContainer(false);
	}

	void JsonWriter::EndArray()
	{
		m_State->EndContainer(false);
	}

	void JsonWriter::WriteKey(std::string_view key)
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.IsFinished, "JsonWriter: WriteKey after Finish");
		const bool isInObject = !state.IsFinished && !state.Stack.empty() && state.Stack.back().IsObject;
		ENGINE_CORE_ASSERT(isInObject, "JsonWriter: WriteKey outside an object");
		if (!isInObject)
			return;
		State::Container& object = state.Stack.back();
		ENGINE_CORE_ASSERT(!object.PendingKey.has_value(), "JsonWriter: two keys without a value between them");

		const std::string printableKey = Utils::SanitizedUtf8(key);
		const std::string pointer = JsonReader::AppendPointer(object.Pointer, printableKey);
		if (!IsValidUtf8(key))
			state.RecordError("invalid UTF-8 in a key", pointer);
		if (!object.Keys.emplace(key).second)
			state.RecordError(std::format("duplicate key '{}'", printableKey), pointer);
		object.PendingKey = std::string(key);
	}

	void JsonWriter::WriteNull()
	{
		m_State->AddScalar("null");
	}

	void JsonWriter::WriteBool(bool value)
	{
		m_State->AddScalar(value ? "true" : "false");
	}

	void JsonWriter::WriteInt(int64_t value)
	{
		m_State->AddScalar(Utils::NumberText(value));
	}

	void JsonWriter::WriteUInt(uint64_t value)
	{
		m_State->AddScalar(Utils::NumberText(value));
	}

	void JsonWriter::WriteFloat(float value)
	{
		State& state = *m_State;
		if (!std::isfinite(value))
		{
			if (state.CanWriteValue())
			{
				state.RecordError(std::format("{} cannot be written: JSON numbers are finite", value), state.ValuePointer());
				state.AddValue("null", false);
			}
			return;
		}
		// -0 is written as 0; std::to_chars on float gives the shortest text that round-trips the value.
		state.AddScalar(value == 0.0f ? std::string("0") : Utils::NumberText(value));
	}

	void JsonWriter::WriteString(std::string_view value)
	{
		State& state = *m_State;
		if (!state.CanWriteValue())
			return;
		if (!IsValidUtf8(value))
		{
			state.RecordError("invalid UTF-8 in a string", state.ValuePointer());
			state.AddValue(Utils::QuoteString(Utils::SanitizedUtf8(value)), false);
			return;
		}
		state.AddValue(Utils::QuoteString(value), false);
	}

	void JsonWriter::WriteUUID(UUID value)
	{
		WriteString(std::format("{}", value));
	}

	void JsonWriter::WriteJson(const Json& value)
	{
		// Iterative, so the writer itself never recurses per level; the bound protects the code that copies and destroys
		// trees, which does.
		struct Frame
		{
			const Json* Container = nullptr;
			Json::const_iterator Next;
		};
		std::vector<Frame> frames;

		const auto writeValue = [this, &frames](const Json& item)
		{
			switch (item.type())
			{
				case Json::value_t::object:
				case Json::value_t::array:
				{
					if (item.is_object())
						BeginObject();
					else
						BeginArray();
					frames.push_back(Frame{ .Container = &item, .Next = item.cbegin() });
					ENGINE_CORE_ASSERT(frames.size() <= MaxJsonDepth, "JsonWriter::WriteJson: the tree nests deeper than {} levels", MaxJsonDepth);
					break;
				}
				case Json::value_t::null:
					WriteNull();
					break;
				case Json::value_t::boolean:
					WriteBool(item.get<bool>());
					break;
				case Json::value_t::number_integer:
					WriteInt(item.get<int64_t>());
					break;
				case Json::value_t::number_unsigned:
					WriteUInt(item.get<uint64_t>());
					break;
				case Json::value_t::number_float:
				{
					const auto number = item.get<double>();
					if (std::isfinite(number) && !(std::fabs(number) < Utils::FloatRoundingLimit))
					{
						if (m_State->CanWriteValue())
						{
							m_State->RecordError(std::format("{} is out of range for float", number), m_State->ValuePointer());
							m_State->AddValue("null", false);
						}
						break;
					}
					// A non-finite value is reported by WriteFloat.
					WriteFloat(static_cast<float>(number));
					break;
				}
				case Json::value_t::string:
					WriteString(item.get_ref<const std::string&>());
					break;
				case Json::value_t::binary:
				case Json::value_t::discarded:
				{
					ENGINE_CORE_ASSERT(false, "JsonWriter::WriteJson: a {} value has no JSON text", item.type_name());
					if (m_State->CanWriteValue())
					{
						m_State->RecordError(std::format("a {} value has no JSON text", item.type_name()), m_State->ValuePointer());
						m_State->AddValue("null", false);
					}
					break;
				}
			}
		};

		writeValue(value);
		while (!frames.empty())
		{
			Frame& frame = frames.back();
			if (frame.Next == frame.Container->cend())
			{
				if (frame.Container->is_object())
					EndObject();
				else
					EndArray();
				frames.pop_back();
				continue;
			}
			const Json& child = *frame.Next;
			if (frame.Container->is_object())
				WriteKey(frame.Next.key());
			++frame.Next;
			writeValue(child); // may grow `frames`, so `frame` is not used after this
		}
	}

	Result<std::string> JsonWriter::Finish()
	{
		State& state = *m_State;
		ENGINE_CORE_ASSERT(!state.IsFinished, "JsonWriter::Finish called twice");
		ENGINE_CORE_ASSERT(state.Stack.empty(), "JsonWriter::Finish with {} open containers", state.Stack.size());
		ENGINE_CORE_ASSERT(state.HasRoot, "JsonWriter::Finish before a root value was written");
		if (state.IsFinished || !state.Stack.empty() || !state.HasRoot)
			return MakeError(ErrorCode::InvalidState, "JsonWriter::Finish on an incomplete document");
		state.IsFinished = true;

		if (state.FirstError)
			return std::unexpected(std::move(*state.FirstError));
		if (state.IsPretty())
			state.Output += '\n';
		return std::move(state.Output);
	}

	Result<std::string> JsonWriter::Write(const Json& value, JsonStyle style)
	{
		JsonWriter writer(style);
		writer.WriteJson(value);
		return writer.Finish();
	}

}
