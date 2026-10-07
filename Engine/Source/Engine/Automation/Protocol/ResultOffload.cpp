#include "EnginePCH.h"
#include "Engine/Automation/Protocol/ResultOffload.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"

#include <format>

namespace Engine {

	namespace Utils {

		// The most bytes the described members of a summary take; the rest is left for "omittedMembers" and the braces, so the
		// whole summary stays below 4 KB.
		constexpr size_t MaxSummaryMemberBytes = 3584;

		// The description of one value: containers and strings by size, scalars by value.
		[[nodiscard]] static Json DescribeValue(const Json& value)
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

	}

	std::string MakeOffloadServerTag(uint32_t processId, int64_t startSeconds)
	{
		return std::format("{}-{}", processId, startSeconds);
	}

	std::string MakeOffloadFileName(std::string_view serverTag, uint64_t sequence)
	{
		ENGINE_CORE_ASSERT(!serverTag.empty(), "An offload file name needs a server tag");
		return std::format("{}-{:08}.json", serverTag, sequence);
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
			Json description = Utils::DescribeValue(member.value());
			// The member's share of the minified summary: "key":value, and a comma.
			const size_t size = member.key().size() + description.dump(-1, ' ', false, Json::error_handler_t::replace).size() + 4;
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
