#include "EnginePCH.h"
#include "Engine/Automation/Protocol/JsonReference.h"

#include "Engine/Core/Json/JsonReader.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Utils {

		// A decimal number without sign or leading zeros ("0" itself allowed), or nullopt.
		[[nodiscard]] static std::optional<size_t> ParseReferenceIndex(std::string_view text)
		{
			if (text.empty() || text.size() > 9 || (text.size() > 1 && text.front() == '0'))
				return std::nullopt;
			size_t value = 0;
			for (const char character : text)
			{
				if (character < '0' || character > '9')
					return std::nullopt;
				value = value * 10 + static_cast<size_t>(character - '0');
			}
			return value;
		}

		// What a batch numbers its requests by: ops from 0 (edit.batch), lines from 1 (batch files).
		[[nodiscard]] static std::string_view DescribeReferenceUnit(size_t indexBase)
		{
			return indexBase == 0 ? "op" : "line";
		}

		// An InvalidArgument error located at the "$ref" object at `pointer`.
		[[nodiscard]] static std::unexpected<Error> MakeReferenceError(const std::string& pointer, std::string message)
		{
			ErrorLocation location;
			location.JsonPointer = pointer;
			return std::unexpected(Error(ErrorCode::InvalidArgument, message)
					.WithLocation(std::move(location))
					.WithIssue(ErrorIssue{ .JsonPointer = pointer, .Message = std::move(message), .Hint = {}, .Suggestions = {} }));
		}

		// The value `reference` ("<index>.<path>") names in `results`.
		[[nodiscard]] static Result<Json> ResolveReference(std::string_view reference, std::span<const Json> results, size_t indexBase,
			const std::string& pointer)
		{
			const std::string_view unit = DescribeReferenceUnit(indexBase);
			const size_t dot = reference.find('.');
			const std::optional<size_t> index = ParseReferenceIndex(reference.substr(0, dot));
			if (!index.has_value())
			{
				return MakeReferenceError(pointer, std::format("malformed $ref '{}': expected \"<{}>.<path>\" with a decimal {} number", reference, unit, unit));
			}
			if (*index < indexBase || *index - indexBase >= results.size())
			{
				return MakeReferenceError(pointer, std::format("$ref '{}' names {} {}, which has no result yet: only earlier {}s can be "
															   "referenced",
													   reference, unit, *index, unit));
			}

			const Json* value = &results[*index - indexBase];
			if (dot == std::string_view::npos)
				return *value;
			std::string_view path = reference.substr(dot + 1);
			if (path.empty())
				return MakeReferenceError(pointer, std::format("malformed $ref '{}': empty path after '.'", reference));
			while (true)
			{
				const size_t next = path.find('.');
				const std::string_view segment = path.substr(0, next);
				if (segment.empty())
					return MakeReferenceError(pointer, std::format("malformed $ref '{}': empty path segment", reference));
				if (value->is_object())
				{
					const auto member = value->find(std::string(segment));
					if (member == value->end())
					{
						return MakeReferenceError(pointer, std::format("$ref '{}': no member '{}' in the result of {} {}", reference, segment, unit, *index));
					}
					value = &*member;
				}
				else if (value->is_array())
				{
					const std::optional<size_t> element = ParseReferenceIndex(segment);
					if (!element.has_value() || *element >= value->size())
					{
						return MakeReferenceError(pointer, std::format("$ref '{}': no element '{}' in an array of {} in the result of {} {}", reference, segment, value->size(), unit, *index));
					}
					value = &(*value)[*element];
				}
				else
				{
					return MakeReferenceError(pointer, std::format("$ref '{}': '{}' is inside a {}, which has no members, in the result of {} {}", reference, segment, JsonTypeToString(GetJsonType(*value)), unit, *index));
				}
				if (next == std::string_view::npos)
					return *value;
				path.remove_prefix(next + 1);
			}
		}

		// Replaces the references inside `value` (located at `pointer`).
		[[nodiscard]] static Status SubstituteIn(Json& value, std::span<const Json> results, size_t indexBase, const std::string& pointer)
		{
			if (value.is_object())
			{
				if (value.size() == 1)
				{
					const auto reference = value.find("$ref");
					if (reference != value.end() && reference->is_string())
					{
						ENGINE_TRY_ASSIGN(const std::string text, JsonReader(*reference).ReadString());
						ENGINE_TRY_ASSIGN(Json resolved, ResolveReference(text, results, indexBase, pointer));
						value = std::move(resolved);
						return {};
					}
				}
				for (auto member = value.begin(); member != value.end(); ++member)
					ENGINE_TRY(SubstituteIn(member.value(), results, indexBase, JsonReader::AppendPointer(pointer, member.key())));
				return {};
			}
			if (value.is_array())
			{
				for (size_t index = 0; index < value.size(); ++index)
					ENGINE_TRY(SubstituteIn(value[index], results, indexBase, JsonReader::AppendPointer(pointer, index)));
			}
			return {};
		}

	}

	Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase)
	{
		// Substitution works on a copy, so a failure leaves `value` unchanged.
		Json substituted = value;
		ENGINE_TRY(Utils::SubstituteIn(substituted, results, indexBase, std::string()));
		value = std::move(substituted);
		return {};
	}

}
