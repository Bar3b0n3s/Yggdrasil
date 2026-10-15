#include "EnginePCH.h"
#include "Engine/Automation/Protocol/ResultOffload.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <algorithm>
#include <charconv>
#include <format>
#include <system_error>

namespace Engine {

	namespace Utils {

		// The most bytes the described members of a summary take; the rest is left for "omittedMembers" and the braces, so the
		// whole summary stays below 4 KB.
		constexpr size_t MaxSummaryMemberBytes = 3584;

		[[nodiscard]] static bool IsDecimal(std::string_view text)
		{
			return !text.empty() && std::ranges::all_of(text, [](char character)
			{
				return character >= '0' && character <= '9';
			});
		}

		[[nodiscard]] static bool IsExtension(std::string_view text)
		{
			return !text.empty() && std::ranges::all_of(text, [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
			});
		}

		[[nodiscard]] static size_t MeasureMinified(const Json& value)
		{
			return value.dump(-1, ' ', false, Json::error_handler_t::replace).size();
		}

		// The description of one value by its type and size alone: {"type", "count"} for a container, {"type": "string",
		// "length"} for a string, the value itself for a scalar.
		[[nodiscard]] static Json DescribeSize(const Json& value)
		{
			const JsonType type = GetJsonType(value);
			switch (type)
			{
				case JsonType::Array:
				case JsonType::Object:
				{
					Json description = Json::object();
					description["type"] = std::string(JsonTypeToString(type));
					description["count"] = value.size();
					return description;
				}
				case JsonType::String:
				{
					Json description = Json::object();
					description["type"] = "string";
					description["length"] = JsonReader(value).ReadString().value_or(std::string()).size();
					return description;
				}
				case JsonType::Null:
				case JsonType::Bool:
				case JsonType::Integer:
				case JsonType::Float:
					return value;
			}
			return value;
		}

		// The description of one value (ResultOffload.h): the value itself when its minified JSON is short; an array of
		// strings with its leading strings that fit MaxOffloadSummaryListBytes ({"type", "count", "first"}); else by its type
		// and size (DescribeSize).
		[[nodiscard]] static Json DescribeValue(const Json& value)
		{
			const JsonType type = GetJsonType(value);
			if ((type == JsonType::Array || type == JsonType::Object || type == JsonType::String) && MeasureMinified(value) <= MaxOffloadSummaryVerbatimBytes)
				return value;
			Json description = DescribeSize(value);
			const bool strings = type == JsonType::Array && !value.empty() && std::ranges::all_of(value, [](const Json& element)
			{
				return element.is_string();
			});
			if (!strings)
				return description;
			Json first = Json::array();
			size_t used = 2; // the brackets
			for (const Json& element : value)
			{
				const size_t size = MeasureMinified(element) + 1; // and a comma
				if (used + size > MaxOffloadSummaryListBytes)
					break;
				used += size;
				first.push_back(element);
			}
			description["first"] = std::move(first);
			return description;
		}

	}

	std::string MakeOffloadServerTag(uint32_t processId, int64_t startSeconds)
	{
		return std::format("{}-{}", processId, startSeconds);
	}

	std::string MakeOffloadFileName(std::string_view serverTag, uint64_t sequence, std::string_view extension)
	{
		ENGINE_CORE_ASSERT(!serverTag.empty(), "An offload file name needs a server tag");
		ENGINE_CORE_ASSERT(Utils::IsExtension(extension), "offload file extension '{}' must be lowercase letters and digits", extension);
		return std::format("{}-{:08}.{}", serverTag, sequence, extension);
	}

	std::optional<uint32_t> ParseOffloadFileProcessId(std::string_view fileName)
	{
		// "<processId>-<startSeconds>-<sequence>.<extension>[.bak | .tmp-<suffix>]"
		std::string_view name = fileName;
		if (const size_t temporary = name.find(".tmp-"); temporary != std::string_view::npos)
			name = name.substr(0, temporary);
		else if (name.ends_with(".bak"))
			name.remove_suffix(4);
		const size_t dot = name.find('.');
		if (dot == std::string_view::npos || !Utils::IsExtension(name.substr(dot + 1)))
			return std::nullopt;
		const std::string_view stem = name.substr(0, dot);
		const size_t first = stem.find('-');
		const size_t second = first == std::string_view::npos ? std::string_view::npos : stem.find('-', first + 1);
		if (second == std::string_view::npos || !Utils::IsDecimal(stem.substr(0, first)) || !Utils::IsDecimal(stem.substr(first + 1, second - first - 1))
			|| !Utils::IsDecimal(stem.substr(second + 1)))
			return std::nullopt;
		uint32_t processId = 0;
		const std::from_chars_result parsed = std::from_chars(stem.data(), stem.data() + first, processId);
		if (parsed.ec != std::errc() || parsed.ptr != stem.data() + first)
			return std::nullopt;
		return processId;
	}

	Json MakeOffloadSummary(const Json& result)
	{
		if (!result.is_object())
		{
			// Results are objects (MakeResultResponse); anything else is described by its type and size alone.
			Json description = Utils::DescribeValue(result);
			if (description.is_object())
				return description;
			Json scalar = Json::object();
			scalar["type"] = std::string(JsonTypeToString(GetJsonType(result)));
			scalar["count"] = 1;
			return scalar;
		}

		Json summary = Json::object();
		size_t used = 0;
		size_t omitted = 0;
		for (auto member = result.begin(); member != result.end(); ++member)
		{
			// The member's share of the minified summary: "key":value, and a comma.
			const auto measure = [&member](const Json& description)
			{
				return member.key().size() + Utils::MeasureMinified(description) + 4;
			};
			Json description = Utils::DescribeValue(member.value());
			size_t size = measure(description);
			const bool listed = member.value().is_array() && description.is_object() && description.contains("first");
			if (omitted == 0 && used + size > Utils::MaxSummaryMemberBytes && listed)
			{
				// A list's leading strings that do not fit leave room for the members after it: its size alone.
				description = Utils::DescribeSize(member.value());
				size = measure(description);
			}
			if (omitted > 0 || used + size > Utils::MaxSummaryMemberBytes)
			{
				++omitted;
				continue;
			}
			used += size;
			summary[member.key()] = std::move(description);
		}
		if (omitted > 0)
			summary["omittedMembers"] = omitted;
		return summary;
	}

	Json MakeOffloadedResult(std::string_view path, const Json& summary)
	{
		Json replacement = Json::object();
		replacement["path"] = std::string(path);
		replacement["truncated"] = true;
		replacement["summary"] = summary;
		return replacement;
	}

}
