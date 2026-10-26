#include "EnginePCH.h"
#include "Engine/Automation/Protocol/JsonReference.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FuzzySuggest.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		// The earlier result a reference names: its position in the results, and how messages name its op or line ("op 3",
		// "line 2", or "op 'board'" for a reference by name).
		struct ReferencedResult
		{
			size_t Position = 0;
			std::string Description{};
		};

	}

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

		// An op's name (EditBatchOp::Name): [A-Za-z_] followed by [A-Za-z0-9_], so it never reads as an index.
		[[nodiscard]] static bool IsReferenceName(std::string_view text)
		{
			const auto isLetter = [](char character)
			{
				return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') || character == '_';
			};
			if (text.empty() || !isLetter(text.front()))
				return false;
			return std::ranges::all_of(text, [&isLetter](char character)
			{
				return isLetter(character) || (character >= '0' && character <= '9');
			});
		}

		// What a batch numbers its requests by: ops from 0 (edit.batch), lines from 1 (batch files).
		[[nodiscard]] static std::string_view DescribeReferenceUnit(size_t indexBase)
		{
			return indexBase == 0 ? "op" : "line";
		}

		// An InvalidArgument error located at the "$ref" object at `pointer`, with an optional hint and suggestions.
		[[nodiscard]] static std::unexpected<Error> MakeReferenceError(const std::string& pointer, std::string message, std::string hint = {},
			std::vector<std::string> suggestions = {})
		{
			ErrorLocation location;
			location.JsonPointer = pointer;
			return std::unexpected(Error(ErrorCode::InvalidArgument, message)
					.WithHint(hint)
					.WithLocation(std::move(location))
					.WithIssue(ErrorIssue{ .JsonPointer = pointer, .Message = std::move(message), .Hint = std::move(hint), .Suggestions = std::move(suggestions) }));
		}

		// The most members or names a hint lists; it counts the rest ("and 12 more").
		constexpr size_t MaxListedReferenceMembers = 32;

		// `texts` quoted and separated by commas, at most MaxListedReferenceMembers of them, then how many more.
		[[nodiscard]] static std::string ListQuoted(std::span<const std::string> texts)
		{
			std::string listed;
			for (size_t index = 0; index < texts.size() && index < MaxListedReferenceMembers; ++index)
				listed += std::format("{}'{}'", listed.empty() ? "" : ", ", texts[index]);
			if (texts.size() > MaxListedReferenceMembers)
				listed += std::format(" and {} more", texts.size() - MaxListedReferenceMembers);
			return listed;
		}

		// The error of a path segment naming no member of `object`, which `inspected` names: the reference up to the segment,
		// the result itself ("0") or an object inside it ("0.prefab"). Its hint names the members the object has, sorted, at
		// most MaxListedReferenceMembers of them, after "did you mean" for close ones, which are also the issue's suggestions.
		[[nodiscard]] static std::unexpected<Error> MakeMissingMemberError(const std::string& pointer, std::string_view reference,
			std::string_view inspected, std::string_view segment, const Json& object, std::string_view referenced)
		{
			std::vector<std::string> members;
			members.reserve(object.size());
			for (auto member = object.begin(); member != object.end(); ++member)
				members.push_back(member.key());
			std::ranges::sort(members);
			std::vector<std::string> suggestions = FuzzySuggest(segment, members);
			const std::string where = inspected.contains('.') ? std::format("'{}' (in the result of {})", inspected, referenced)
															  : std::format("the result of {}", referenced);
			std::string hint = members.empty() ? std::format("{} is an empty object", where) : std::format("{} has {}", where, ListQuoted(members));
			if (!suggestions.empty())
				hint = std::format("{} {}", MakeDidYouMeanHint(suggestions), hint);
			return MakeReferenceError(pointer, std::format("$ref '{}': no member '{}' in {}", reference, segment, where), std::move(hint), std::move(suggestions));
		}

		// The earlier result `reference`'s first part ("<index>", or with `names` also "<name>") names. `names`, when given,
		// holds every op's name in op order ("" for an unnamed op), aligned with `results`, which holds the earlier ops'.
		[[nodiscard]] static Result<ReferencedResult> FindReferencedResult(std::string_view reference, std::span<const Json> results, size_t indexBase,
			std::optional<std::span<const std::string>> names, const std::string& pointer)
		{
			const std::string_view unit = DescribeReferenceUnit(indexBase);
			const std::string_view head = reference.substr(0, reference.find('.'));
			if (const std::optional<size_t> index = ParseReferenceIndex(head); index.has_value())
			{
				if (*index < indexBase || *index - indexBase >= results.size())
				{
					return MakeReferenceError(pointer, std::format("$ref '{}' names {} {}, which has no result yet: only earlier {}s can be "
																   "referenced",
														   reference, unit, *index, unit));
				}
				return ReferencedResult{ .Position = *index - indexBase, .Description = std::format("{} {}", unit, *index) };
			}
			if (!names.has_value())
			{
				return MakeReferenceError(pointer, std::format("malformed $ref '{}': expected \"<{}>.<path>\" with a decimal {} number", reference, unit, unit));
			}
			if (!IsReferenceName(head))
			{
				return MakeReferenceError(pointer, std::format("malformed $ref '{}': expected \"<{} index or name>.<path>\": a decimal number, or a "
															   "name of letters, digits and _ that does not start with a digit",
													   reference, unit));
			}

			// Only earlier ops have results; a name of this op or a later one is a forward reference.
			const std::span<const std::string> all = *names;
			const size_t earlier = std::min(results.size(), all.size());
			for (size_t position = 0; position < all.size(); ++position)
			{
				if (all[position] != head)
					continue;
				if (position < earlier)
					return ReferencedResult{ .Position = position, .Description = std::format("{} '{}'", unit, head) };
				const size_t number = position + indexBase;
				if (position == results.size())
				{
					return MakeReferenceError(pointer, std::format("$ref '{}': '{}' names this {} ({} {}): only earlier {}s can be referenced", reference, head, unit, unit, number, unit));
				}
				return MakeReferenceError(pointer, std::format("$ref '{}': '{}' names {} {}, which runs after this one: only earlier {}s can be "
															   "referenced",
													   reference, head, unit, number, unit));
			}

			std::vector<std::string> earlierNames;
			for (size_t position = 0; position < earlier; ++position)
			{
				if (!all[position].empty())
					earlierNames.push_back(all[position]);
			}
			std::vector<std::string> suggestions = FuzzySuggest(head, earlierNames);
			std::string hint = earlierNames.empty() ? std::format("no earlier {} has a name: give that {} a \"name\", or reference it by its index", unit, unit)
													: std::format("the earlier {}s are named {}", unit, ListQuoted(earlierNames));
			if (!suggestions.empty())
				hint = std::format("{} {}", MakeDidYouMeanHint(suggestions), hint);
			return MakeReferenceError(pointer, std::format("$ref '{}': no {} named '{}' before this one", reference, unit, head), std::move(hint),
				std::move(suggestions));
		}

		// The value `reference` ("<index>.<path>", or with `names` "<name>.<path>") names in `results`.
		[[nodiscard]] static Result<Json> ResolveReference(std::string_view reference, std::span<const Json> results, size_t indexBase,
			std::optional<std::span<const std::string>> names, const std::string& pointer)
		{
			ENGINE_TRY_ASSIGN(const ReferencedResult referenced, FindReferencedResult(reference, results, indexBase, names, pointer));
			const Json* value = &results[referenced.Position];
			const size_t dot = reference.find('.');
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
						// The reference up to the segment: what was inspected ("0" for the result, "0.prefab" inside it).
						const std::string_view inspected = reference.substr(0, reference.size() - path.size() - 1);
						return MakeMissingMemberError(pointer, reference, inspected, segment, *value, referenced.Description);
					}
					value = &*member;
				}
				else if (value->is_array())
				{
					const std::optional<size_t> element = ParseReferenceIndex(segment);
					if (!element.has_value() || *element >= value->size())
					{
						return MakeReferenceError(pointer, std::format("$ref '{}': no element '{}' in an array of {} in the result of {}", reference, segment, value->size(), referenced.Description));
					}
					value = &(*value)[*element];
				}
				else
				{
					return MakeReferenceError(pointer, std::format("$ref '{}': '{}' is inside a {}, which has no members, in the result of {}", reference, segment, JsonTypeToString(GetJsonType(*value)), referenced.Description));
				}
				if (next == std::string_view::npos)
					return *value;
				path.remove_prefix(next + 1);
			}
		}

		// Replaces the references inside `value` (located at `pointer`).
		[[nodiscard]] static Status SubstituteIn(Json& value, std::span<const Json> results, size_t indexBase, std::optional<std::span<const std::string>> names,
			const std::string& pointer)
		{
			if (value.is_object())
			{
				if (value.size() == 1)
				{
					const auto reference = value.find("$ref");
					if (reference != value.end() && reference->is_string())
					{
						ENGINE_TRY_ASSIGN(const std::string text, JsonReader(*reference).ReadString());
						ENGINE_TRY_ASSIGN(Json resolved, ResolveReference(text, results, indexBase, names, pointer));
						value = std::move(resolved);
						return {};
					}
				}
				for (auto member = value.begin(); member != value.end(); ++member)
					ENGINE_TRY(SubstituteIn(member.value(), results, indexBase, names, JsonReader::AppendPointer(pointer, member.key())));
				return {};
			}
			if (value.is_array())
			{
				for (size_t index = 0; index < value.size(); ++index)
					ENGINE_TRY(SubstituteIn(value[index], results, indexBase, names, JsonReader::AppendPointer(pointer, index)));
			}
			return {};
		}

	}

	Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase)
	{
		// Substitution works on a copy, so a failure leaves `value` unchanged.
		Json substituted = value;
		ENGINE_TRY(Utils::SubstituteIn(substituted, results, indexBase, std::nullopt, std::string()));
		value = std::move(substituted);
		return {};
	}

	Status SubstituteReferences(Json& value, std::span<const Json> results, size_t indexBase, std::span<const std::string> names)
	{
		ENGINE_CORE_ASSERT(names.size() >= results.size(), "SubstituteReferences: {} names for {} earlier results; names holds every op's", names.size(),
			results.size());
		Json substituted = value;
		ENGINE_TRY(Utils::SubstituteIn(substituted, results, indexBase, names, std::string()));
		value = std::move(substituted);
		return {};
	}

}
