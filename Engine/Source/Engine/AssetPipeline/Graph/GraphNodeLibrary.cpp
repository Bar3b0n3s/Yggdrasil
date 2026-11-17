#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphNodeLibrary.h"

#include "Engine/AssetPipeline/Private/GraphTextLimits.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

// The graph node library (Docs/Decisions/0035-m24-contract.md decision 6) and the rules a graph keeps against it (decision 5).
// The generated kinds come from the registry's canonical Luau signatures, read by the small reader of Luau type expressions
// below (the registry's own text, never external input); the built-ins are the table in MakeBuiltinNodes.

namespace Engine {

	namespace Utils {

		// Script.Call's arguments, Arg1 to Arg4: as many as a graph method takes parameters (M39, decision 2).
		static constexpr size_t ScriptCallArguments = MaxGraphMethodParameters;

		// --- Names ----------------------------------------------------------------------------------------------------

		[[nodiscard]] static bool IsAsciiLetter(char c)
		{
			return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
		}

		[[nodiscard]] static bool IsAsciiDigit(char c)
		{
			return c >= '0' && c <= '9';
		}

		[[nodiscard]] static bool IsAsciiUpper(char c)
		{
			return c >= 'A' && c <= 'Z';
		}

		[[nodiscard]] static bool IsAsciiLower(char c)
		{
			return c >= 'a' && c <= 'z';
		}

		// A Luau name ([A-Za-z_][A-Za-z0-9_]*), which is also one part of a kind.
		[[nodiscard]] static bool IsLuauName(std::string_view text)
		{
			if (text.empty() || (!IsAsciiLetter(text.front()) && text.front() != '_'))
				return false;
			return std::ranges::all_of(text, [](char c)
			{
				return IsAsciiLetter(c) || IsAsciiDigit(c) || c == '_';
			});
		}

		// "GetAxis" as "Get Axis", "FindByID" as "Find By ID": a space before an upper-case letter that follows a lower-case
		// one or that starts a word after an acronym.
		[[nodiscard]] static std::string SplitWords(std::string_view name)
		{
			std::string words;
			for (size_t i = 0; i < name.size(); ++i)
			{
				const char c = name[i];
				if (i > 0 && IsAsciiUpper(c))
				{
					const char previous = name[i - 1];
					const bool nextLower = i + 1 < name.size() && IsAsciiLower(name[i + 1]);
					if (IsAsciiLower(previous) || (IsAsciiUpper(previous) && nextLower))
						words += ' ';
				}
				words += c;
			}
			return words;
		}

		// An RFC 6901 reference token.
		[[nodiscard]] static std::string EscapePointerToken(std::string_view token)
		{
			std::string escaped;
			for (const char c : token)
			{
				if (c == '~')
					escaped += "~0";
				else if (c == '/')
					escaped += "~1";
				else
					escaped += c;
			}
			return escaped;
		}

		// --- The registry's Luau type expressions ---------------------------------------------------------------------

		[[nodiscard]] static std::string_view TrimLuau(std::string_view text)
		{
			const size_t first = text.find_first_not_of(" \t\n");
			if (first == std::string_view::npos)
				return {};
			const size_t last = text.find_last_not_of(" \t\n");
			return text.substr(first, last - first + 1);
		}

		// Whether text[index] closes a bracket: ')', '}', ']', or a '>' that is not the end of "->".
		[[nodiscard]] static bool IsLuauCloser(std::string_view text, size_t index)
		{
			const char c = text[index];
			if (c == '>')
				return index == 0 || text[index - 1] != '-';
			return c == ')' || c == '}' || c == ']';
		}

		// Walks `text`, tracking the nesting of (), {}, [] and <> (the '>' of "->" closes nothing) and skipping string
		// literals (singleton types), and calls `visit(index, depth)` for each byte outside a literal and for each literal's
		// opening quote, `depth` being the nesting outside the byte (an opening bracket's before it, a closing one's after
		// it). A true result stops the walk. Returns false for text whose brackets or quotes do not balance.
		template<typename Visit>
		[[nodiscard]] static bool WalkLuauType(std::string_view text, Visit&& visit)
		{
			size_t depth = 0;
			for (size_t i = 0; i < text.size(); ++i)
			{
				const char c = text[i];
				if (c == '"' || c == '\'')
				{
					size_t end = i + 1;
					while (end < text.size() && text[end] != c)
						end += text[end] == '\\' ? 2 : 1;
					if (end >= text.size())
						return false;
					if (visit(i, depth))
						return true;
					i = end;
					continue;
				}
				if (IsLuauCloser(text, i))
				{
					if (depth == 0)
						return false;
					--depth;
				}
				if (visit(i, depth))
					return true;
				if (c == '(' || c == '{' || c == '[' || c == '<')
					++depth;
			}
			return depth == 0;
		}

		// The index of the first `token` at nesting depth 0, or npos (also for unbalanced text).
		[[nodiscard]] static size_t FindLuauToken(std::string_view text, std::string_view token)
		{
			size_t found = std::string_view::npos;
			const bool balanced = WalkLuauType(text, [&](size_t index, size_t depth)
			{
				if (depth == 0 && text.substr(index).starts_with(token))
				{
					found = index;
					return true;
				}
				return false;
			});
			return balanced ? found : std::string_view::npos;
		}

		// The index of the bracket that closes text[0], or npos.
		[[nodiscard]] static size_t FindClosingBracket(std::string_view text)
		{
			size_t found = std::string_view::npos;
			const bool balanced = WalkLuauType(text, [&](size_t index, size_t depth)
			{
				if (index > 0 && depth == 0 && IsLuauCloser(text, index))
				{
					found = index;
					return true;
				}
				return false;
			});
			return balanced ? found : std::string_view::npos;
		}

		// `text` split at the `separator` bytes at nesting depth 0, each part trimmed; nullopt for unbalanced text.
		[[nodiscard]] static std::optional<std::vector<std::string_view>> SplitLuau(std::string_view text, char separator)
		{
			std::vector<std::string_view> parts;
			size_t start = 0;
			const bool balanced = WalkLuauType(text, [&](size_t index, size_t depth)
			{
				if (depth == 0 && text[index] == separator)
				{
					parts.push_back(TrimLuau(text.substr(start, index - start)));
					start = index + 1;
				}
				return false;
			});
			if (!balanced)
				return std::nullopt;
			parts.push_back(TrimLuau(text.substr(start)));
			return parts;
		}

		// What a Luau type is as a pin: not modelled (a function type, a generic, an overload intersection, a singleton or a
		// name the registry does not declare) or a pin type, optional for T? and T | nil. A table type and a union of types
		// other than nil are Any.
		struct ModelledType
		{
			bool Modelled = false;
			GraphPinType Type = GraphPinType::Any;
			std::string TypeName{};
			bool Optional = false;
		};

		// The registry's names: enums and the extern types and aliases, which are Object pins.
		struct LuauTypeNames
		{
			std::map<std::string, std::vector<std::string>, std::less<>> Enums{};
			std::set<std::string, std::less<>> Objects{};
		};

		[[nodiscard]] static ModelledType ClassifyLuauType(std::string_view text, const LuauTypeNames& names)
		{
			ModelledType result;
			text = TrimLuau(text);
			// Optional markers and enclosing parentheses: "(A | B)?", "T?".
			while (!text.empty())
			{
				if (text.back() == '?')
				{
					result.Optional = true;
					text = TrimLuau(text.substr(0, text.size() - 1));
				}
				else if (text.front() == '(' && FindClosingBracket(text) == text.size() - 1)
					text = TrimLuau(text.substr(1, text.size() - 2));
				else
					break;
			}
			if (text.empty() || text.front() == '<' || text.front() == '"' || text.front() == '\'' || FindLuauToken(text, "->") != std::string_view::npos)
				return result;
			const std::optional<std::vector<std::string_view>> intersection = SplitLuau(text, '&');
			if (!intersection.has_value() || intersection->size() > 1)
				return result;
			const std::optional<std::vector<std::string_view>> alternatives = SplitLuau(text, '|');
			if (!alternatives.has_value())
				return result;
			if (alternatives->size() > 1)
			{
				std::vector<std::string_view> types;
				for (const std::string_view part : *alternatives)
				{
					if (part == "nil")
						result.Optional = true;
					else
						types.push_back(part);
				}
				if (types.empty())
					return result;
				if (types.size() == 1)
				{
					ModelledType single = ClassifyLuauType(types.front(), names);
					single.Optional = single.Optional || result.Optional;
					return single;
				}
				for (const std::string_view part : types)
				{
					if (!ClassifyLuauType(part, names).Modelled)
						return result;
				}
				result.Modelled = true;
				result.Type = GraphPinType::Any;
				return result;
			}
			if (text.front() == '{')
			{
				result.Modelled = true;
				result.Type = GraphPinType::Any;
				return result;
			}
			if (!IsLuauName(text))
				return result;
			static constexpr std::pair<std::string_view, GraphPinType> Builtins[] = {
				{ "number", GraphPinType::Number },
				{ "boolean", GraphPinType::Bool },
				{ "string", GraphPinType::String },
				{ "vector", GraphPinType::Vector },
				{ "any", GraphPinType::Any },
				{ "unknown", GraphPinType::Any },
				{ "Color", GraphPinType::Color },
				{ "Quat", GraphPinType::Quat },
				{ "Entity", GraphPinType::Entity },
				{ "AssetRef", GraphPinType::Asset },
			};
			for (const auto& [name, type] : Builtins)
			{
				if (text == name)
				{
					result.Modelled = true;
					result.Type = type;
					return result;
				}
			}
			if (names.Enums.contains(text))
			{
				result.Modelled = true;
				result.Type = GraphPinType::Enum;
				result.TypeName = std::string(text);
			}
			else if (names.Objects.contains(text))
			{
				result.Modelled = true;
				result.Type = GraphPinType::Object;
				result.TypeName = std::string(text);
			}
			return result;
		}

		struct LuauParameter
		{
			std::string_view Name{};
			std::string_view Type{};
			bool Variadic = false;
		};

		struct LuauFunction
		{
			std::vector<LuauParameter> Parameters{};
			std::vector<std::string_view> Results{};
		};

		// One function type "(<parameters>) -> <results>"; nullopt for a generic, an overload intersection, an unnamed
		// parameter, a variadic parameter that is not last or variadic results.
		[[nodiscard]] static std::optional<LuauFunction> ParseLuauFunction(std::string_view signature)
		{
			const std::string_view text = TrimLuau(signature);
			if (text.empty() || text.front() != '(')
				return std::nullopt;
			const size_t close = FindClosingBracket(text);
			if (close == std::string_view::npos)
				return std::nullopt;
			const std::string_view rest = TrimLuau(text.substr(close + 1));
			if (!rest.starts_with("->"))
				return std::nullopt;
			LuauFunction function;
			const std::string_view parameters = TrimLuau(text.substr(1, close - 1));
			if (!parameters.empty())
			{
				const std::optional<std::vector<std::string_view>> parts = SplitLuau(parameters, ',');
				if (!parts.has_value())
					return std::nullopt;
				for (size_t i = 0; i < parts->size(); ++i)
				{
					const std::string_view part = (*parts)[i];
					if (part.starts_with("..."))
					{
						if (i + 1 != parts->size())
							return std::nullopt;
						function.Parameters.push_back({ .Name = {}, .Type = TrimLuau(part.substr(3)), .Variadic = true });
						continue;
					}
					const size_t colon = FindLuauToken(part, ":");
					if (colon == std::string_view::npos)
						return std::nullopt;
					const std::string_view name = TrimLuau(part.substr(0, colon));
					if (!IsLuauName(name))
						return std::nullopt;
					function.Parameters.push_back({ .Name = name, .Type = TrimLuau(part.substr(colon + 1)), .Variadic = false });
				}
			}
			const std::string_view results = TrimLuau(rest.substr(2));
			if (results.starts_with("..."))
				return std::nullopt;
			if (!results.empty() && results.front() == '(' && FindClosingBracket(results) == results.size() - 1 && FindLuauToken(results, "->") == std::string_view::npos)
			{
				const std::string_view inner = TrimLuau(results.substr(1, results.size() - 2));
				if (!inner.empty())
				{
					const std::optional<std::vector<std::string_view>> parts = SplitLuau(inner, ',');
					if (!parts.has_value())
						return std::nullopt;
					for (const std::string_view part : *parts)
					{
						if (part.starts_with("..."))
							return std::nullopt;
						function.Results.push_back(part);
					}
				}
			}
			else if (!results.empty())
				function.Results.push_back(results);
			return function;
		}

		// --- Pins -----------------------------------------------------------------------------------------------------

		// The literal an unlinked required input of `type` takes, or null for a type without one (decision 6).
		[[nodiscard]] static VariantValue MakePinDefault(GraphPinType type, std::string_view typeName, const LuauTypeNames& names)
		{
			switch (type)
			{
				case GraphPinType::Bool:   return VariantValue(Json(false));
				case GraphPinType::Number: return VariantValue(Json(0));
				case GraphPinType::String: return VariantValue(Json(""));
				case GraphPinType::Vector: return VariantValue(Json::array({ 0, 0, 0 }));
				case GraphPinType::Color:  return VariantValue(Json::array({ 1, 1, 1, 1 }));
				case GraphPinType::Quat:   return VariantValue(Json::array({ 0, 0, 0, 1 }));
				case GraphPinType::Enum:
				{
					const auto found = names.Enums.find(typeName);
					if (found != names.Enums.end() && !found->second.empty())
						return VariantValue(Json(found->second.front()));
					return {};
				}
				default: return {};
			}
		}

		[[nodiscard]] static GraphPinDescription MakeExecPin(std::string name, GraphPinDirection direction)
		{
			return { .Name = std::move(name), .Direction = direction, .Type = GraphPinType::Exec, .TypeName = {}, .Optional = false, .Default = {} };
		}

		[[nodiscard]] static GraphPinDescription MakeInputPin(std::string name, GraphPinType type, std::string typeName, bool optional, const LuauTypeNames& names)
		{
			GraphPinDescription pin{ .Name = std::move(name), .Direction = GraphPinDirection::Input, .Type = type, .TypeName = std::move(typeName), .Optional = optional, .Default = {} };
			if (!optional)
				pin.Default = MakePinDefault(pin.Type, pin.TypeName, names);
			return pin;
		}

		[[nodiscard]] static GraphPinDescription MakeOutputPin(std::string name, GraphPinType type, std::string typeName, bool optional)
		{
			return { .Name = std::move(name), .Direction = GraphPinDirection::Output, .Type = type, .TypeName = std::move(typeName), .Optional = optional, .Default = {} };
		}

		[[nodiscard]] static GraphPinDescription MakeInputPin(std::string name, const ModelledType& type, const LuauTypeNames& names)
		{
			return MakeInputPin(std::move(name), type.Type, type.TypeName, type.Optional, names);
		}

		[[nodiscard]] static GraphPinDescription MakeOutputPin(std::string name, const ModelledType& type)
		{
			return MakeOutputPin(std::move(name), type.Type, type.TypeName, type.Optional);
		}

		// Orders `pins` as the header states: the exec input, the data inputs, the exec outputs, the data outputs, each group
		// in the order given.
		static void OrderPins(std::vector<GraphPinDescription>& pins)
		{
			const auto rank = [](const GraphPinDescription& pin)
			{
				const bool exec = pin.Type == GraphPinType::Exec;
				if (pin.Direction == GraphPinDirection::Input)
					return exec ? 0 : 1;
				return exec ? 2 : 3;
			};
			std::ranges::stable_sort(pins, {}, rank);
		}

		// --- Generated kinds ------------------------------------------------------------------------------------------

		// Available in every run mode and at run time (decision 6's eligibility).
		[[nodiscard]] static bool IsEligibleMember(const ScriptApiMember& member)
		{
			return member.Options.Modes == RunModes::All && HasFlag(member.Options.Environments, ScriptApiEnvironment::Runtime);
		}

		[[nodiscard]] static std::string MakeResultName(size_t index, size_t count)
		{
			return count == 1 ? std::string("Result") : std::format("Result{}", index + 1);
		}

		// Call.<Group>.<Member> from a function, method or constructor; nullopt when the signature is not modelled.
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeCallNode(const ScriptApiGroup& group, const ScriptApiMember& member, const LuauTypeNames& names)
		{
			const std::optional<LuauFunction> function = ParseLuauFunction(member.Signature);
			if (!function.has_value())
				return std::nullopt;
			GraphNodeDescription node;
			node.Kind = std::format("Call.{}.{}", group.Name, member.Name);
			node.Title = SplitWords(member.Name);
			node.Category = GraphNodeCategory::Call;
			node.Description = member.Description;
			node.Pure = !member.Options.Mutates && !function->Results.empty();
			for (const LuauParameter& parameter : function->Parameters)
			{
				const ModelledType type = ClassifyLuauType(parameter.Type, names);
				if (!type.Modelled)
					return std::nullopt;
				// A trailing variadic is one optional input "Values", passed as its one argument.
				if (parameter.Variadic)
					node.Pins.push_back(MakeInputPin("Values", type.Type, type.TypeName, true, names));
				else
					node.Pins.push_back(MakeInputPin(std::string(parameter.Name), type, names));
			}
			for (size_t i = 0; i < function->Results.size(); ++i)
			{
				const ModelledType type = ClassifyLuauType(function->Results[i], names);
				if (!type.Modelled)
					return std::nullopt;
				node.Pins.push_back(MakeOutputPin(MakeResultName(i, function->Results.size()), type));
			}
			if (!node.Pure)
			{
				node.Pins.push_back(MakeExecPin("In", GraphPinDirection::Input));
				node.Pins.push_back(MakeExecPin("Then", GraphPinDirection::Output));
			}
			OrderPins(node.Pins);
			return node;
		}

		// The "self" input of a type's property or method: the type's own pin.
		[[nodiscard]] static std::optional<GraphPinDescription> MakeSelfPin(std::string_view typeName, const LuauTypeNames& names)
		{
			const ModelledType type = ClassifyLuauType(typeName, names);
			if (!type.Modelled || type.Optional)
				return std::nullopt;
			return MakeInputPin("self", type.Type, type.TypeName, false, names);
		}

		// Get.<Group>.<Member>: a getter, pure.
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeGetNode(std::string_view group, std::string_view member, std::string_view valueType,
			std::string description, const std::optional<GraphPinDescription>& self, const LuauTypeNames& names)
		{
			const ModelledType type = ClassifyLuauType(valueType, names);
			if (!type.Modelled)
				return std::nullopt;
			GraphNodeDescription node;
			node.Kind = std::format("Get.{}.{}", group, member);
			node.Title = "Get " + SplitWords(member);
			node.Category = GraphNodeCategory::Property;
			node.Description = std::move(description);
			node.Pure = true;
			if (self.has_value())
				node.Pins.push_back(*self);
			node.Pins.push_back(MakeOutputPin("Value", type));
			return node;
		}

		// Set.<Group>.<Member>: a setter, a statement.
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeSetNode(std::string_view group, std::string_view member, std::string_view valueType,
			std::string description, const std::optional<GraphPinDescription>& self, const LuauTypeNames& names)
		{
			const ModelledType type = ClassifyLuauType(valueType, names);
			if (!type.Modelled)
				return std::nullopt;
			GraphNodeDescription node;
			node.Kind = std::format("Set.{}.{}", group, member);
			node.Title = "Set " + SplitWords(member);
			node.Category = GraphNodeCategory::Property;
			node.Description = std::move(description);
			node.Pure = false;
			node.Pins.push_back(MakeExecPin("In", GraphPinDirection::Input));
			if (self.has_value())
				node.Pins.push_back(*self);
			node.Pins.push_back(MakeInputPin("Value", type, names));
			node.Pins.push_back(MakeExecPin("Then", GraphPinDirection::Output));
			return node;
		}

		// Const.<Group>.<Member>, pure.
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeConstNode(const ScriptApiGroup& group, const ScriptApiMember& member, const LuauTypeNames& names)
		{
			const ModelledType type = ClassifyLuauType(member.Signature, names);
			if (!type.Modelled)
				return std::nullopt;
			GraphNodeDescription node;
			node.Kind = std::format("Const.{}.{}", group.Name, member.Name);
			node.Title = SplitWords(member.Name);
			node.Category = GraphNodeCategory::Constant;
			node.Description = member.Description;
			node.Pure = true;
			node.Pins.push_back(MakeOutputPin("Value", type));
			return node;
		}

		// Event.<Callback>: Then and the callback's parameters, by name (self is the instance, the function's own).
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeEventNode(const ScriptApiMember& member, const LuauTypeNames& names)
		{
			const std::optional<LuauFunction> function = ParseLuauFunction(member.Signature);
			if (!function.has_value())
				return std::nullopt;
			GraphNodeDescription node;
			node.Kind = std::format("Event.{}", member.Name);
			node.Title = SplitWords(member.Name);
			node.Category = GraphNodeCategory::Event;
			node.Description = member.Description;
			node.Pure = false;
			node.Pins.push_back(MakeExecPin("Then", GraphPinDirection::Output));
			for (size_t i = 0; i < function->Parameters.size(); ++i)
			{
				const LuauParameter& parameter = function->Parameters[i];
				if (i == 0 && parameter.Name == "self")
					continue;
				if (parameter.Variadic)
					return std::nullopt;
				const ModelledType type = ClassifyLuauType(parameter.Type, names);
				if (!type.Modelled)
					return std::nullopt;
				node.Pins.push_back(MakeOutputPin(std::string(parameter.Name), type));
			}
			return node;
		}

		// Return.<Callback> of a callback with one modelled result (M39, decision 4: the OnUi* callbacks' boolean?): In, no exec
		// output, and Value of the result's type; nullopt for a callback without a result.
		[[nodiscard]] static std::optional<GraphNodeDescription> MakeCallbackReturnNode(const ScriptApiMember& member, const LuauTypeNames& names)
		{
			const std::optional<LuauFunction> function = ParseLuauFunction(member.Signature);
			if (!function.has_value() || function->Results.size() != 1)
				return std::nullopt;
			const ModelledType type = ClassifyLuauType(function->Results.front(), names);
			if (!type.Modelled)
				return std::nullopt;
			const bool consumes = type.Type == GraphPinType::Bool && type.Optional;
			GraphNodeDescription node;
			node.Kind = std::format("{}{}", GraphReturnKindPrefix, member.Name);
			node.Title = std::format("Return {}", SplitWords(member.Name));
			node.Category = GraphNodeCategory::Flow;
			node.Description = consumes ? std::format("Ends the {} callback with Value as its result: true consumes the UI event, nil (unset) lets it bubble on.", member.Name)
										: std::format("Ends the {} callback with Value as its result.", member.Name);
			node.Pure = false;
			node.Pins.push_back(MakeExecPin("In", GraphPinDirection::Input));
			node.Pins.push_back(MakeInputPin("Value", type, names));
			return node;
		}

		// The record fields of an alias ("{Point: vector, Normal: vector}") as (name, type); empty for any other alias.
		[[nodiscard]] static std::vector<std::pair<std::string_view, std::string_view>> GetRecordFields(std::string_view definition)
		{
			std::vector<std::pair<std::string_view, std::string_view>> fields;
			const std::string_view text = TrimLuau(definition);
			if (text.empty() || text.front() != '{' || FindClosingBracket(text) != text.size() - 1)
				return fields;
			const std::optional<std::vector<std::string_view>> parts = SplitLuau(text.substr(1, text.size() - 2), ',');
			if (!parts.has_value())
				return fields;
			for (const std::string_view part : *parts)
			{
				const size_t colon = FindLuauToken(part, ":");
				if (colon == std::string_view::npos)
					continue;
				const std::string_view name = TrimLuau(part.substr(0, colon));
				if (IsLuauName(name))
					fields.emplace_back(name, TrimLuau(part.substr(colon + 1)));
			}
			return fields;
		}

		// --- Built-in kinds -------------------------------------------------------------------------------------------

		[[nodiscard]] static GraphNodeDescription MakeBuiltin(std::string kind, std::string title, GraphNodeCategory category, std::string description, bool pure,
			std::vector<GraphPinDescription> pins)
		{
			GraphNodeDescription node{ .Kind = std::move(kind), .Title = std::move(title), .Category = category, .Description = std::move(description), .Pure = pure, .Pins = std::move(pins) };
			OrderPins(node.Pins);
			return node;
		}

		// The built-ins of decision 6.
		[[nodiscard]] static std::vector<GraphNodeDescription> MakeBuiltinNodes(const LuauTypeNames& names)
		{
			const auto in = []()
			{
				return MakeExecPin("In", GraphPinDirection::Input);
			};
			const auto out = [](std::string name)
			{
				return MakeExecPin(std::move(name), GraphPinDirection::Output);
			};
			const auto data = [&names](std::string name, GraphPinType type)
			{
				return MakeInputPin(std::move(name), type, {}, false, names);
			};
			const auto result = [](std::string name, GraphPinType type)
			{
				return MakeOutputPin(std::move(name), type, {}, false);
			};
			using enum GraphPinType;
			std::vector<GraphNodeDescription> nodes;
			nodes.push_back(MakeBuiltin("Flow.Branch", "Branch", GraphNodeCategory::Flow, "Runs True when Condition holds and False otherwise.", false,
				{ in(), data("Condition", Bool), out("True"), out("False") }));
			nodes.push_back(MakeBuiltin("Flow.Sequence", "Sequence", GraphNodeCategory::Flow, "Runs Then1, Then2, Then3 and Then4 in order.", false,
				{ in(), out("Then1"), out("Then2"), out("Then3"), out("Then4") }));
			nodes.push_back(MakeBuiltin("Flow.ForRange", "For Range", GraphNodeCategory::Flow,
				"Runs Body once for each whole Index from First to Last, then Completed.", false,
				{ in(), data("First", Number), data("Last", Number), out("Body"), result("Index", Number), out("Completed") }));
			nodes.push_back(MakeBuiltin("Flow.ForEach", "For Each", GraphNodeCategory::Flow,
				"Runs Body once for each Element of the array, with its 1-based Index, then Completed.", false,
				{ in(), data("Array", Any), out("Body"), result("Element", Any), result("Index", Number), out("Completed") }));
			nodes.push_back(MakeBuiltin("Flow.Delay", "Delay", GraphNodeCategory::Flow, "Continues at Then at once and at Later after Seconds of simulation time.",
				false, { in(), data("Seconds", Number), out("Then"), out("Later") }));
			nodes.push_back(MakeBuiltin("Flow.IfNotNil", "If Not Nil", GraphNodeCategory::Flow,
				"Runs True when Value is not nil, with Result the value every input takes, and False otherwise.", false,
				{ in(), data("Value", Any), out("True"), out("False"), result("Result", Any) }));
			nodes.push_back(MakeBuiltin("Self.Entity", "Self Entity", GraphNodeCategory::Member, "The entity this behaviour instance is on.", true,
				{ result("Value", Entity) }));
			// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 2): this and another behaviour's members, and any table's.
			nodes.push_back(MakeBuiltin("Self.Instance", "Self Instance", GraphNodeCategory::Member,
				"This behaviour instance (self): pass it to a method, or call its own methods with Script.Call.", true, { result("Value", Any) }));
			nodes.push_back(MakeBuiltin("Script.Get", "Get Member", GraphNodeCategory::Member,
				"The member Member of Object (a behaviour instance or a table): nil when a table lacks it; an engine value without it raises an error.", true,
				{ data("Object", Any), data("Member", String), result("Value", Any) }));
			nodes.push_back(MakeBuiltin("Script.Set", "Set Member", GraphNodeCategory::Member, "Sets the member Member of Object (a behaviour instance or a table) to Value.",
				false, { in(), data("Object", Any), data("Member", String), data("Value", Any), out("Then") }));
			{
				std::vector<GraphPinDescription> pins{ in(), data("Object", Any), data("Method", String) };
				for (size_t argument = 1; argument <= ScriptCallArguments; ++argument)
					pins.push_back(MakeInputPin(std::format("Arg{}", argument), Any, {}, true, names));
				pins.push_back(out("Then"));
				pins.push_back(result("Result", Any));
				nodes.push_back(MakeBuiltin("Script.Call", "Call Method", GraphNodeCategory::Member,
					"Calls the method Method of Object (a behaviour instance or a table) with Arg1 to Arg4, and gives its first result; calling a method Object "
					"lacks raises an error.",
					false, std::move(pins)));
			}
			for (const auto& [name, title, description] : std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>>{
					 { "Add", "Add", "A + B." },
					 { "Subtract", "Subtract", "A - B." },
					 { "Multiply", "Multiply", "A * B." },
					 { "Divide", "Divide", "A / B." },
					 { "Modulo", "Modulo", "A modulo B (Luau's %: the result has the sign of B)." } })
			{
				nodes.push_back(MakeBuiltin(std::format("Number.{}", name), std::string(title), GraphNodeCategory::Operator, std::string(description), true,
					{ data("A", Number), data("B", Number), result("Result", Number) }));
			}
			nodes.push_back(MakeBuiltin("Number.Negate", "Negate", GraphNodeCategory::Operator, "-Value.", true, { data("Value", Number), result("Result", Number) }));
			nodes.push_back(MakeBuiltin("Compare.Equal", "Equal", GraphNodeCategory::Operator, "Whether A equals B (Luau's ==).", true,
				{ data("A", Any), data("B", Any), result("Result", Bool) }));
			nodes.push_back(MakeBuiltin("Compare.NotEqual", "Not Equal", GraphNodeCategory::Operator, "Whether A differs from B (Luau's ~=).", true,
				{ data("A", Any), data("B", Any), result("Result", Bool) }));
			for (const auto& [name, title, description] : std::initializer_list<std::tuple<std::string_view, std::string_view, std::string_view>>{
					 { "Less", "Less", "A < B." },
					 { "LessEqual", "Less Or Equal", "A <= B." },
					 { "Greater", "Greater", "A > B." },
					 { "GreaterEqual", "Greater Or Equal", "A >= B." } })
			{
				nodes.push_back(MakeBuiltin(std::format("Compare.{}", name), std::string(title), GraphNodeCategory::Operator, std::string(description), true,
					{ data("A", Number), data("B", Number), result("Result", Bool) }));
			}
			nodes.push_back(MakeBuiltin("Logic.And", "And", GraphNodeCategory::Operator, "A and B.", true, { data("A", Bool), data("B", Bool), result("Result", Bool) }));
			nodes.push_back(MakeBuiltin("Logic.Or", "Or", GraphNodeCategory::Operator, "A or B.", true, { data("A", Bool), data("B", Bool), result("Result", Bool) }));
			nodes.push_back(MakeBuiltin("Logic.Not", "Not", GraphNodeCategory::Operator, "Not Value.", true, { data("Value", Bool), result("Result", Bool) }));
			nodes.push_back(MakeBuiltin("Vector.Make", "Make Vector", GraphNodeCategory::Operator, "The vector (X, Y, Z).", true,
				{ data("X", Number), data("Y", Number), data("Z", Number), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Vector.Split", "Split Vector", GraphNodeCategory::Operator, "The components of Value.", true,
				{ data("Value", Vector), result("X", Number), result("Y", Number), result("Z", Number) }));
			nodes.push_back(MakeBuiltin("Vector.Add", "Add Vectors", GraphNodeCategory::Operator, "A + B, component-wise.", true,
				{ data("A", Vector), data("B", Vector), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Vector.Subtract", "Subtract Vectors", GraphNodeCategory::Operator, "A - B, component-wise.", true,
				{ data("A", Vector), data("B", Vector), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Vector.Cross", "Cross", GraphNodeCategory::Operator, "The cross product of A and B.", true,
				{ data("A", Vector), data("B", Vector), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Vector.Scale", "Scale Vector", GraphNodeCategory::Operator, "Value * Factor.", true,
				{ data("Value", Vector), data("Factor", Number), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Vector.Dot", "Dot", GraphNodeCategory::Operator, "The dot product of A and B.", true,
				{ data("A", Vector), data("B", Vector), result("Result", Number) }));
			nodes.push_back(MakeBuiltin("Vector.Length", "Length", GraphNodeCategory::Operator, "The length of Value.", true,
				{ data("Value", Vector), result("Result", Number) }));
			nodes.push_back(MakeBuiltin("Vector.Normalize", "Normalize", GraphNodeCategory::Operator, "Value scaled to length 1.", true,
				{ data("Value", Vector), result("Result", Vector) }));
			nodes.push_back(MakeBuiltin("Text.Concat", "Concat", GraphNodeCategory::Operator, "A and B as text (tostring), joined.", true,
				{ data("A", Any), data("B", Any), result("Result", String) }));
			return nodes;
		}

		// The pin type of a member of kind `kind`: Integer as Number, Enum as String, Array as Any, Asset as Asset with its
		// AssetType (decision 6).
		[[nodiscard]] static std::pair<GraphPinType, std::string> GetMemberPinType(GraphValueKind kind, std::string_view assetType)
		{
			switch (kind)
			{
				case GraphValueKind::Number:
				case GraphValueKind::Integer: return { GraphPinType::Number, {} };
				case GraphValueKind::Bool:    return { GraphPinType::Bool, {} };
				case GraphValueKind::String:
				case GraphValueKind::Enum:    return { GraphPinType::String, {} };
				case GraphValueKind::Vector:  return { GraphPinType::Vector, {} };
				case GraphValueKind::Color:   return { GraphPinType::Color, {} };
				case GraphValueKind::Quat:    return { GraphPinType::Quat, {} };
				case GraphValueKind::Entity:  return { GraphPinType::Entity, {} };
				case GraphValueKind::Asset:   return { GraphPinType::Asset, std::string(assetType) };
				case GraphValueKind::Array:
				case GraphValueKind::Any:     return { GraphPinType::Any, {} };
			}
			return { GraphPinType::Any, {} };
		}

		// Self.Get.<Name> and Self.Set.<Name> of one field or variable.
		[[nodiscard]] static std::pair<GraphNodeDescription, GraphNodeDescription> MakeMemberNodes(std::string_view name, GraphValueKind kind, std::string_view assetType,
			bool field)
		{
			const LuauTypeNames none;
			const auto [type, typeName] = GetMemberPinType(kind, assetType);
			const std::string_view what = field ? "field" : "variable";
			GraphNodeDescription get{ .Kind = std::format("Self.Get.{}", name),
				.Title = std::format("Get {}", name),
				.Category = GraphNodeCategory::Member,
				.Description = std::format("The value of the behaviour's {} {}.", what, name),
				.Pure = true,
				.Pins = { MakeOutputPin("Value", type, typeName, false) } };
			GraphNodeDescription set{ .Kind = std::format("Self.Set.{}", name),
				.Title = std::format("Set {}", name),
				.Category = GraphNodeCategory::Member,
				.Description = std::format("Sets the behaviour's {} {}.", what, name),
				.Pure = false,
				.Pins = { MakeExecPin("In", GraphPinDirection::Input), MakeInputPin("Value", type, typeName, false, none), MakeExecPin("Then", GraphPinDirection::Output) } };
			return { std::move(get), std::move(set) };
		}

		// Method.<Name> and Return.<Name> of one graph method (M39, decision 3): the event with Then and the parameters, typed by
		// their kinds (never optional), and the statement with In and, for a method with a result, Value (optional for Any).
		[[nodiscard]] static std::pair<GraphNodeDescription, GraphNodeDescription> MakeMethodNodes(const GraphMethod& method)
		{
			const LuauTypeNames none;
			std::string parameters;
			GraphNodeDescription event{ .Kind = std::format("{}{}", GraphMethodKindPrefix, method.Name),
				.Title = std::format("Method {}", method.Name),
				.Category = GraphNodeCategory::Event,
				.Description = {},
				.Pure = false,
				.Pins = { MakeExecPin("Then", GraphPinDirection::Output) } };
			for (const GraphParameter& parameter : method.Parameters)
			{
				const auto [type, typeName] = GetMemberPinType(parameter.Kind, parameter.AssetType);
				event.Pins.push_back(MakeOutputPin(parameter.Name, type, typeName, false));
				parameters += std::format("{}{}", parameters.empty() ? "" : ", ", parameter.Name);
			}
			event.Description = std::format("Runs when a script calls the behaviour's method {}({}); its outputs are the call's arguments.", method.Name, parameters);
			GraphNodeDescription result{ .Kind = std::format("{}{}", GraphReturnKindPrefix, method.Name),
				.Title = std::format("Return {}", method.Name),
				.Category = GraphNodeCategory::Flow,
				.Description = method.Results.empty() ? std::format("Ends the method {}.", method.Name) : std::format("Ends the method {} with Value as its result.", method.Name),
				.Pure = false,
				.Pins = { MakeExecPin("In", GraphPinDirection::Input) } };
			if (!method.Results.empty())
			{
				const GraphResult& returned = method.Results.front();
				const auto [type, typeName] = GetMemberPinType(returned.Kind, returned.AssetType);
				result.Pins.push_back(MakeInputPin("Value", type, typeName, returned.Kind == GraphValueKind::Any, none));
			}
			return { std::move(event), std::move(result) };
		}

		// The canonical listing of a kind, which GetFingerprint hashes.
		static void AppendListing(std::string& listing, const GraphNodeDescription& node)
		{
			listing += std::format("{}\n{}\n{}\n{}\n{}\n", node.Kind, node.Title, static_cast<int>(node.Category), node.Pure ? 1 : 0, node.Description);
			for (const GraphPinDescription& pin : node.Pins)
			{
				listing += std::format("\t{}\t{}\t{}\t{}\t{}\t", pin.Name, static_cast<int>(pin.Direction), GraphPinTypeToString(pin.Type), pin.TypeName, pin.Optional ? 1 : 0);
				if (const Json& value = pin.Default.Get(); value.is_string())
					listing += JsonReader(value).ReadString().value_or(std::string());
				else if (value.is_boolean())
					listing += JsonReader(value).ReadBool().value_or(false) ? "true" : "false";
				else if (value.is_number())
					listing += std::format("{}", JsonReader(value).ReadDouble().value_or(0.0));
				else if (value.is_array())
				{
					for (const Json& element : value)
						listing += std::format("{},", JsonReader(element).ReadDouble().value_or(0.0));
				}
				listing += '\n';
			}
		}

	}

	std::string_view GraphPinTypeToString(GraphPinType type)
	{
		switch (type)
		{
			case GraphPinType::Exec:   return "Exec";
			case GraphPinType::Any:    return "Any";
			case GraphPinType::Bool:   return "Bool";
			case GraphPinType::Number: return "Number";
			case GraphPinType::String: return "String";
			case GraphPinType::Vector: return "Vector";
			case GraphPinType::Color:  return "Color";
			case GraphPinType::Quat:   return "Quat";
			case GraphPinType::Entity: return "Entity";
			case GraphPinType::Asset:  return "Asset";
			case GraphPinType::Enum:   return "Enum";
			case GraphPinType::Object: return "Object";
		}
		return "Unknown";
	}

	bool AreGraphPinsCompatible(const GraphPinDescription& from, const GraphPinDescription& to)
	{
		const bool fromExec = from.Type == GraphPinType::Exec;
		const bool toExec = to.Type == GraphPinType::Exec;
		if (fromExec || toExec)
			return fromExec && toExec;
		if (from.Type == GraphPinType::Any || to.Type == GraphPinType::Any)
			return true;
		// A T? output may give nil, which the strict checker lets reach only an optional input.
		if (from.Optional && !to.Optional)
			return false;
		if (from.Type == GraphPinType::Enum && to.Type == GraphPinType::String)
			return true;
		if (from.Type != to.Type)
			return false;
		if (from.Type == GraphPinType::Enum || from.Type == GraphPinType::Object)
			return from.TypeName == to.TypeName;
		return true;
	}

	struct GraphNodeLibrary::State
	{
		std::vector<GraphNodeDescription> Nodes{}; // sorted by Kind
		std::map<std::string, std::vector<std::string>, std::less<>> Enums{};
		uint64_t Fingerprint = 0;
	};

	GraphNodeLibrary::GraphNodeLibrary(ConstructionKey /*key*/)
		: m_State(CreateScope<State>())
	{
	}

	GraphNodeLibrary::~GraphNodeLibrary() = default;

	Result<Scope<GraphNodeLibrary>> GraphNodeLibrary::Create(const ScriptApiRegistry& api)
	{
		if (!api.IsFrozen())
			return MakeError(ErrorCode::InvalidState, "the graph node library needs a frozen script API registry");
		Utils::LuauTypeNames names;
		for (const ScriptApiEnum& enumeration : api.GetEnums())
		{
			std::vector<std::string>& choices = names.Enums[enumeration.Name];
			for (const EnumEntry& entry : enumeration.Values)
				choices.push_back(entry.Name);
		}
		for (const ScriptApiGroup& type : api.GetTypes())
			names.Objects.insert(type.Name);
		for (const ScriptApiAlias& alias : api.GetAliases())
			names.Objects.insert(alias.Name);

		std::vector<GraphNodeDescription> nodes = Utils::MakeBuiltinNodes(names);
		const auto add = [&nodes](std::optional<GraphNodeDescription> node)
		{
			if (node.has_value())
				nodes.push_back(std::move(*node));
		};
		const auto addGroup = [&add, &names](const ScriptApiGroup& group, bool type)
		{
			if (!Utils::IsLuauName(group.Name))
				return;
			const std::optional<GraphPinDescription> self = type ? Utils::MakeSelfPin(group.Name, names) : std::nullopt;
			for (const ScriptApiMember& member : group.Members)
			{
				if (!Utils::IsEligibleMember(member) || !Utils::IsLuauName(member.Name))
					continue;
				switch (member.Kind)
				{
					case ScriptApiMemberKind::Function:
					case ScriptApiMemberKind::Method:
					case ScriptApiMemberKind::Constructor: add(Utils::MakeCallNode(group, member, names)); break;
					case ScriptApiMemberKind::Property:
					case ScriptApiMemberKind::ProxyField:
						// A type's property reads and writes its object; a module's has none.
						if (type && !self.has_value())
							break;
						if (member.Readable)
							add(Utils::MakeGetNode(group.Name, member.Name, member.Signature, member.Description, self, names));
						if (member.Writable)
							add(Utils::MakeSetNode(group.Name, member.Name, member.Signature, member.Description, self, names));
						break;
					case ScriptApiMemberKind::Constant: add(Utils::MakeConstNode(group, member, names)); break;
					case ScriptApiMemberKind::Callback:
						if (type && group.Name == "Behaviour")
						{
							add(Utils::MakeEventNode(member, names));
							add(Utils::MakeCallbackReturnNode(member, names));
						}
						break;
					case ScriptApiMemberKind::Operator: break;
				}
			}
		};
		for (const ScriptApiGroup& module : api.GetModules())
			addGroup(module, false);
		for (const ScriptApiGroup& type : api.GetTypes())
			addGroup(type, true);
		for (const ScriptApiAlias& alias : api.GetAliases())
		{
			if (!Utils::IsLuauName(alias.Name))
				continue;
			const std::optional<GraphPinDescription> self = Utils::MakeInputPin("self", GraphPinType::Object, alias.Name, false, names);
			for (const auto& [field, fieldType] : Utils::GetRecordFields(alias.Definition))
				add(Utils::MakeGetNode(alias.Name, field, fieldType, alias.Description, self, names));
		}

		std::ranges::stable_sort(nodes, {}, &GraphNodeDescription::Kind);
		for (size_t i = 1; i < nodes.size(); ++i)
		{
			if (nodes[i].Kind == nodes[i - 1].Kind)
				return MakeError(ErrorCode::Validation, "two script API members give the graph node kind '{}'", nodes[i].Kind);
		}
		Result<Scope<GraphNodeLibrary>> library = CreateScope<GraphNodeLibrary>(ConstructionKey());
		std::string listing;
		for (const GraphNodeDescription& node : nodes)
			Utils::AppendListing(listing, node);
		(*library)->m_State->Fingerprint = XXH64(listing);
		(*library)->m_State->Nodes = std::move(nodes);
		(*library)->m_State->Enums = std::move(names.Enums);
		return library;
	}

	std::span<const GraphNodeDescription> GraphNodeLibrary::GetNodes() const
	{
		return m_State->Nodes;
	}

	const GraphNodeDescription* GraphNodeLibrary::Find(std::string_view kind) const
	{
		const auto found = std::ranges::lower_bound(m_State->Nodes, kind, std::less<>(), &GraphNodeDescription::Kind);
		return found != m_State->Nodes.end() && found->Kind == kind ? &*found : nullptr;
	}

	Result<GraphNodeDescription> GraphNodeLibrary::Resolve(std::string_view kind, const GraphDocument& graph) const
	{
		if (const GraphNodeDescription* node = Find(kind))
			return *node;
		std::vector<GraphNodeDescription> members = GetMemberNodes(graph);
		const auto member = std::ranges::find(members, kind, &GraphNodeDescription::Kind);
		if (member != members.end())
			return std::move(*member);
		std::vector<std::string_view> candidates;
		candidates.reserve(m_State->Nodes.size() + members.size());
		for (const GraphNodeDescription& node : m_State->Nodes)
			candidates.push_back(node.Kind);
		for (const GraphNodeDescription& node : members)
			candidates.push_back(node.Kind);
		std::vector<std::string> suggestions = FuzzySuggest(kind, candidates, 3);
		std::string hint = MakeDidYouMeanHint(suggestions);
		const std::string message = std::format("unknown graph node kind '{}'", kind);
		return std::unexpected(Error(ErrorCode::NotFound, message)
				.WithHint(hint)
				.WithIssues({ ErrorIssue{ .JsonPointer = {}, .Message = message, .Hint = hint, .Suggestions = std::move(suggestions) } }));
	}

	std::vector<GraphNodeDescription> GraphNodeLibrary::GetMemberNodes(const GraphDocument& graph) const
	{
		std::vector<GraphNodeDescription> nodes;
		nodes.reserve(2 * (graph.Fields.size() + graph.Variables.size() + graph.Methods.size()));
		for (const GraphField& field : graph.Fields)
		{
			auto [get, set] = Utils::MakeMemberNodes(field.Name, field.Kind, field.AssetType, true);
			nodes.push_back(std::move(get));
			nodes.push_back(std::move(set));
		}
		for (const GraphVariable& variable : graph.Variables)
		{
			auto [get, set] = Utils::MakeMemberNodes(variable.Name, variable.Kind, variable.AssetType, false);
			nodes.push_back(std::move(get));
			nodes.push_back(std::move(set));
		}
		for (const GraphMethod& method : graph.Methods)
		{
			auto [event, result] = Utils::MakeMethodNodes(method);
			nodes.push_back(std::move(event));
			nodes.push_back(std::move(result));
		}
		std::ranges::stable_sort(nodes, {}, &GraphNodeDescription::Kind);
		return nodes;
	}

	uint64_t GraphNodeLibrary::GetFingerprint() const
	{
		return m_State->Fingerprint;
	}

	std::span<const std::string> GraphNodeLibrary::GetEnumChoices(std::string_view enumName) const
	{
		const auto found = m_State->Enums.find(enumName);
		if (found == m_State->Enums.end())
			return {};
		return found->second;
	}

	namespace Utils {

		// What ValidateGraph and FindUnreachableGraphNodes know of a graph: each node's kind resolved against the library,
		// and the links that join an existing output to an existing input of compatible pins, at most one into a data input
		// and one out of an exec output (the "valid" links, which the flow and data rules follow).
		class GraphAnalysis
		{
		public:
			struct Link
			{
				size_t Index = 0; // into GraphDocument::Links
				size_t From = 0;  // node indexes
				size_t To = 0;
				const GraphPinDescription* FromPin = nullptr; // borrowed from Descriptions
				const GraphPinDescription* ToPin = nullptr;
			};

			struct Issue
			{
				size_t Section = 0; // 0 the nodes, 1 the links
				size_t Index = 0;
				size_t Part = 0; // a node's Kind 0, its Values 1
				std::string Pin{};
				GraphIssue Value{};
			};

			GraphAnalysis(const GraphDocument& graph, const GraphNodeLibrary& library)
				: m_Graph(graph), m_Library(library)
			{
				m_Members = library.GetMemberNodes(graph);
				m_Descriptions.resize(graph.Nodes.size());
				m_Ids.reserve(graph.Nodes.size());
				for (size_t i = 0; i < graph.Nodes.size(); ++i)
					m_Ids.emplace_back(graph.Nodes[i].Id, i);
				std::ranges::sort(m_Ids);
				for (size_t i = 0; i < graph.Nodes.size(); ++i)
				{
					const GraphNode& node = graph.Nodes[i];
					if (const GraphNodeDescription* found = library.Find(node.Kind))
					{
						m_Descriptions[i] = *found;
						continue;
					}
					const auto member = std::ranges::lower_bound(m_Members, node.Kind, std::less<>(), &GraphNodeDescription::Kind);
					if (member != m_Members.end() && member->Kind == node.Kind)
					{
						m_Descriptions[i] = *member;
						continue;
					}
					std::string message = std::format("node {}: unknown node kind '{}'", node.Id, node.Kind);
					const std::vector<std::string> suggestions = SuggestKinds(node.Kind, library);
					if (!suggestions.empty())
						message += std::format(" ({})", MakeDidYouMeanHint(suggestions));
					AddNodeIssue(i, 0, {}, "/Kind", std::move(message), false);
				}
				CheckValues();
				CheckLinks();
			}

			[[nodiscard]] const std::optional<GraphNodeDescription>& GetDescription(size_t node) const { return m_Descriptions[node]; }
			[[nodiscard]] std::span<const Link> GetLinks() const { return m_Links; }

			[[nodiscard]] bool IsEvent(size_t node) const
			{
				return m_Descriptions[node].has_value() && m_Descriptions[node]->Category == GraphNodeCategory::Event;
			}

			[[nodiscard]] bool IsStatement(size_t node) const
			{
				return m_Descriptions[node].has_value() && !m_Descriptions[node]->Pure && m_Descriptions[node]->Category != GraphNodeCategory::Event;
			}

			[[nodiscard]] bool IsPure(size_t node) const
			{
				return m_Descriptions[node].has_value() && m_Descriptions[node]->Pure;
			}

			// The node indexes each event's flow reaches through the valid exec links (events included).
			[[nodiscard]] std::vector<bool> FindReachable() const
			{
				const std::vector<std::vector<size_t>> successors = GetExecSuccessors();
				std::vector<bool> reached(m_Graph.Nodes.size(), false);
				std::vector<size_t> pending;
				for (size_t i = 0; i < m_Graph.Nodes.size(); ++i)
				{
					if (IsEvent(i))
					{
						reached[i] = true;
						pending.push_back(i);
					}
				}
				while (!pending.empty())
				{
					const size_t node = pending.back();
					pending.pop_back();
					for (const size_t next : successors[node])
					{
						if (!reached[next])
						{
							reached[next] = true;
							pending.push_back(next);
						}
					}
				}
				return reached;
			}

			// The cycle rules and the completeness rules (decision 5), and where a Return may be reached (M39).
			void CheckCyclesAndCompleteness()
			{
				const bool dataCycle = CheckCycles(false);
				const bool execCycle = CheckCycles(true);
				CheckRequiredInputs();
				if (!dataCycle && !execCycle)
					CheckDominance();
				if (!execCycle)
					CheckReturns();
			}

			// No statement's code starts deeper than MaxGraphTextDepth blocks in the transpiled text, and the text holds at most
			// MaxGraphTextBytes (decision 7, risk 1), measured by laying the text out as TranspileGraph does, so a graph
			// transpiles exactly when it validates. Only a graph whose issues are all Incomplete has that layout, so the edit
			// methods refuse the edit that nests a flow too deep or grows the text too large while a graph is still unfinished.
			void CheckTextLimits()
			{
				const bool laidOut = std::ranges::none_of(m_Issues, [](const Issue& issue)
				{
					return !issue.Value.Incomplete;
				});
				if (!laidOut)
					return;
				const GraphTextLimitIssues limits = FindGraphTextLimitIssues(m_Graph, m_Library);
				for (const size_t node : limits.TooDeep)
					AddNodeIssue(node, 0, {}, {}, DescribeGraphStatementTooDeep(m_Graph.Nodes[node]), false);
				if (limits.TooLarge.has_value())
					AddNodeIssue(*limits.TooLarge, 0, {}, {}, DescribeGraphTextTooLarge(m_Graph.Nodes[*limits.TooLarge]), false);
			}

			[[nodiscard]] std::vector<GraphIssue> TakeIssues()
			{
				std::ranges::stable_sort(m_Issues, [](const Issue& left, const Issue& right)
				{
					return std::tie(left.Section, left.Index, left.Part, left.Pin) < std::tie(right.Section, right.Index, right.Part, right.Pin);
				});
				std::vector<GraphIssue> issues;
				issues.reserve(m_Issues.size());
				for (Issue& issue : m_Issues)
					issues.push_back(std::move(issue.Value));
				return issues;
			}
		private:
			// Up to three close kinds among those of `kind`'s group (the kinds up to its last '.', "Call.Assets." for
			// "Call.Assets.Lode"), so a misspelt member finds its own and a hostile file of thousands of unknown kinds stays
			// cheap (Resolve suggests from the whole library).
			[[nodiscard]] std::vector<std::string> SuggestKinds(std::string_view kind, const GraphNodeLibrary& library) const
			{
				const size_t dot = kind.rfind('.');
				if (dot == std::string_view::npos)
					return {};
				const std::string_view group = kind.substr(0, dot + 1);
				std::vector<std::string_view> candidates;
				const auto collect = [&candidates, group](std::span<const GraphNodeDescription> nodes)
				{
					for (auto it = std::ranges::lower_bound(nodes, group, std::less<>(), &GraphNodeDescription::Kind); it != nodes.end() && it->Kind.starts_with(group); ++it)
						candidates.push_back(it->Kind);
				};
				collect(library.GetNodes());
				collect(m_Members);
				return FuzzySuggest(kind, candidates, 3);
			}

			[[nodiscard]] std::optional<size_t> FindNodeIndex(uint32_t id) const
			{
				const auto found = std::ranges::lower_bound(m_Ids, std::pair<uint32_t, size_t>(id, 0));
				if (found == m_Ids.end() || found->first != id)
					return std::nullopt;
				return found->second;
			}

			[[nodiscard]] static const GraphPinDescription* FindPin(const GraphNodeDescription& node, std::string_view name, GraphPinDirection direction)
			{
				for (const GraphPinDescription& pin : node.Pins)
				{
					if (pin.Name == name && pin.Direction == direction)
						return &pin;
				}
				return nullptr;
			}

			void AddNodeIssue(size_t node, size_t part, std::string pin, std::string_view suffix, std::string message, bool incomplete)
			{
				Issue issue{ .Section = 0, .Index = node, .Part = part, .Pin = std::move(pin), .Value = {} };
				issue.Value = GraphIssue{ .JsonPointer = std::format("/Nodes/{}{}", node, suffix), .Node = m_Graph.Nodes[node].Id, .Message = std::move(message), .Incomplete = incomplete };
				m_Issues.push_back(std::move(issue));
			}

			void AddLinkIssue(size_t link, uint32_t node, std::string message, bool incomplete)
			{
				Issue issue{ .Section = 1, .Index = link, .Part = 0, .Pin = {}, .Value = {} };
				issue.Value = GraphIssue{ .JsonPointer = std::format("/Links/{}", link), .Node = node, .Message = std::move(message), .Incomplete = incomplete };
				m_Issues.push_back(std::move(issue));
			}

			// Whether `value` is a literal `pin` takes (null is no literal and fits every input).
			[[nodiscard]] bool LiteralFits(const Json& value, const GraphPinDescription& pin) const
			{
				if (value.is_null())
					return true;
				const auto numbers = [&value](size_t count)
				{
					if (!value.is_array() || value.size() != count)
						return false;
					return std::ranges::all_of(value, [](const Json& element)
					{
						return element.is_number();
					});
				};
				switch (pin.Type)
				{
					case GraphPinType::Bool:   return value.is_boolean();
					case GraphPinType::Number: return value.is_number();
					case GraphPinType::String: return value.is_string();
					case GraphPinType::Vector: return numbers(3);
					case GraphPinType::Color:  return numbers(4);
					case GraphPinType::Quat:
					{
						// Quat.New refuses the zero quaternion at run time, so a literal is nonzero once its components are
						// rounded to float, as a member's Default (ValidateGraphStructure).
						if (!numbers(4))
							return false;
						return std::ranges::any_of(value, [](const Json& element)
						{
							return static_cast<float>(JsonReader(element).ReadDouble().value_or(0.0)) != 0.0f;
						});
					}
					case GraphPinType::Enum:
					{
						const Result<std::string> choice = JsonReader(value).ReadString();
						if (!choice.has_value())
							return false;
						const std::span<const std::string> choices = m_Library.GetEnumChoices(pin.TypeName);
						return std::ranges::find(choices, *choice) != choices.end();
					}
					case GraphPinType::Asset:
					{
						const Result<std::string> text = JsonReader(value).ReadString();
						if (!text.has_value())
							return false;
						const std::optional<UUID> handle = UUID::FromString(*text);
						return handle.has_value() && handle->IsValid();
					}
					case GraphPinType::Any:    return value.is_boolean() || value.is_number() || value.is_string();
					case GraphPinType::Exec:
					case GraphPinType::Entity:
					case GraphPinType::Object: return false;
				}
				return false;
			}

			// Every value names a data input of its node and fits its type.
			void CheckValues()
			{
				for (size_t i = 0; i < m_Graph.Nodes.size(); ++i)
				{
					if (!m_Descriptions[i].has_value())
						continue;
					const GraphNodeDescription& description = *m_Descriptions[i];
					for (const auto& [name, value] : m_Graph.Nodes[i].Values)
					{
						const std::string suffix = "/Values/" + EscapePointerToken(name);
						const GraphPinDescription* pin = FindPin(description, name, GraphPinDirection::Input);
						if (pin == nullptr || pin->Type == GraphPinType::Exec)
						{
							AddNodeIssue(i, 1, name, suffix, std::format("node {} ({}) has no data input '{}' to hold a value", m_Graph.Nodes[i].Id, description.Kind, name), false);
							continue;
						}
						if (!LiteralFits(value.Get(), *pin))
						{
							std::string expected(GraphPinTypeToString(pin->Type));
							if (pin->Type == GraphPinType::Entity || pin->Type == GraphPinType::Object)
								expected = "a link (it takes no literal)";
							else if (pin->Type == GraphPinType::Enum)
								expected = std::format("one of the choices of {}", pin->TypeName);
							else if (pin->Type == GraphPinType::Asset)
								expected = "a 16-digit hex asset handle or null";
							else if (pin->Type == GraphPinType::Quat)
								expected = "Quat, four numbers that are not all zero";
							AddNodeIssue(i, 1, name, suffix, std::format("node {} ({}): the value of '{}' does not fit its type; expected {}", m_Graph.Nodes[i].Id, description.Kind, name, expected),
								false);
						}
					}
				}
			}

			// Links join an existing output to an existing input of compatible pins; a data input and an exec output take one.
			void CheckLinks()
			{
				std::set<std::pair<size_t, std::string_view>> dataInputs;
				std::set<std::pair<size_t, std::string_view>> execOutputs;
				for (size_t j = 0; j < m_Graph.Links.size(); ++j)
				{
					const GraphLink& link = m_Graph.Links[j];
					const std::optional<size_t> from = FindNodeIndex(link.From.Node);
					const std::optional<size_t> to = FindNodeIndex(link.To.Node);
					if (!from.has_value() || !to.has_value())
					{
						AddLinkIssue(j, link.To.Node, std::format("the link names node {}, which the graph lacks", from.has_value() ? link.To.Node : link.From.Node), false);
						continue;
					}
					m_LinkedInputs.emplace(*to, link.To.Pin);
					if (!m_Descriptions[*from].has_value() || !m_Descriptions[*to].has_value())
						continue; // the unknown kind is the issue
					const GraphNodeDescription& source = *m_Descriptions[*from];
					const GraphNodeDescription& target = *m_Descriptions[*to];
					const GraphPinDescription* fromPin = FindPin(source, link.From.Pin, GraphPinDirection::Output);
					const GraphPinDescription* toPin = FindPin(target, link.To.Pin, GraphPinDirection::Input);
					if (fromPin == nullptr)
					{
						const bool input = FindPin(source, link.From.Pin, GraphPinDirection::Input) != nullptr;
						AddLinkIssue(j, link.From.Node,
							std::format("node {} ({}) has no output '{}'{}", link.From.Node, source.Kind, link.From.Pin, input ? " (it is an input: a link runs from an output to an input)" : ""), false);
						continue;
					}
					if (toPin == nullptr)
					{
						const bool output = FindPin(target, link.To.Pin, GraphPinDirection::Output) != nullptr;
						AddLinkIssue(j, link.To.Node,
							std::format("node {} ({}) has no input '{}'{}", link.To.Node, target.Kind, link.To.Pin, output ? " (it is an output: a link runs from an output to an input)" : ""), false);
						continue;
					}
					if (!AreGraphPinsCompatible(*fromPin, *toPin))
					{
						std::string message = std::format("output '{}' of node {} ({}, {}) cannot feed input '{}' of node {} ({}, {})", link.From.Pin, link.From.Node, source.Kind,
							GraphPinTypeToString(fromPin->Type), link.To.Pin, link.To.Node, target.Kind, GraphPinTypeToString(toPin->Type));
						if (fromPin->Optional && !toPin->Optional && fromPin->Type != GraphPinType::Exec && toPin->Type != GraphPinType::Exec)
							message += ": the output may be nil and the input needs a value; pass it through Flow.IfNotNil and link its Result";
						AddLinkIssue(j, link.To.Node, std::move(message), false);
						continue;
					}
					if (toPin->Type != GraphPinType::Exec && !dataInputs.emplace(*to, toPin->Name).second)
					{
						AddLinkIssue(j, link.To.Node, std::format("input '{}' of node {} already has a link; a data input takes one", link.To.Pin, link.To.Node), false);
						continue;
					}
					if (fromPin->Type == GraphPinType::Exec && !execOutputs.emplace(*from, fromPin->Name).second)
					{
						AddLinkIssue(j, link.From.Node, std::format("exec output '{}' of node {} already has a link; an exec output takes one", link.From.Pin, link.From.Node), false);
						continue;
					}
					m_Links.push_back({ .Index = j, .From = *from, .To = *to, .FromPin = fromPin, .ToPin = toPin });
				}
			}

			[[nodiscard]] std::vector<std::vector<size_t>> GetExecSuccessors() const
			{
				std::vector<std::vector<size_t>> successors(m_Graph.Nodes.size());
				for (const Link& link : m_Links)
				{
					if (link.FromPin->Type == GraphPinType::Exec)
						successors[link.From].push_back(link.To);
				}
				return successors;
			}

			// One issue per cycle of the exec links (`exec`) or of the data links, at the lowest link index of the cycle.
			// Returns whether there is any. Tarjan's components, iterative.
			bool CheckCycles(bool exec)
			{
				const size_t count = m_Graph.Nodes.size();
				std::vector<std::vector<size_t>> successors(count);
				for (const Link& link : m_Links)
				{
					if ((link.FromPin->Type == GraphPinType::Exec) == exec)
						successors[link.From].push_back(link.To);
				}
				constexpr size_t Unvisited = std::numeric_limits<size_t>::max();
				std::vector<size_t> order(count, Unvisited);
				std::vector<size_t> low(count, 0);
				std::vector<size_t> component(count, Unvisited);
				std::vector<bool> onStack(count, false);
				std::vector<size_t> stack;
				std::vector<std::pair<size_t, size_t>> calls; // (node, next successor)
				size_t counter = 0;
				size_t components = 0;
				std::vector<size_t> componentSizes;
				for (size_t start = 0; start < count; ++start)
				{
					if (order[start] != Unvisited)
						continue;
					order[start] = low[start] = counter++;
					stack.push_back(start);
					onStack[start] = true;
					calls.emplace_back(start, 0);
					while (!calls.empty())
					{
						const size_t node = calls.back().first;
						const size_t next = calls.back().second;
						if (next < successors[node].size())
						{
							++calls.back().second;
							const size_t successor = successors[node][next];
							if (order[successor] == Unvisited)
							{
								order[successor] = low[successor] = counter++;
								stack.push_back(successor);
								onStack[successor] = true;
								calls.emplace_back(successor, 0);
							}
							else if (onStack[successor])
								low[node] = std::min(low[node], order[successor]);
							continue;
						}
						if (low[node] == order[node])
						{
							size_t size = 0;
							size_t member = Unvisited;
							do
							{
								member = stack.back();
								stack.pop_back();
								onStack[member] = false;
								component[member] = components;
								++size;
							} while (member != node);
							componentSizes.push_back(size);
							++components;
						}
						calls.pop_back();
						if (!calls.empty())
							low[calls.back().first] = std::min(low[calls.back().first], low[node]);
					}
				}
				std::vector<bool> reported(components, false);
				bool cyclic = false;
				for (const Link& link : m_Links)
				{
					if ((link.FromPin->Type == GraphPinType::Exec) != exec)
						continue;
					const size_t which = component[link.From];
					if (which != component[link.To] || componentSizes[which] < 2 || reported[which])
						continue;
					reported[which] = true;
					cyclic = true;
					std::vector<uint32_t> ids;
					for (size_t i = 0; i < count; ++i)
					{
						if (component[i] == which)
							ids.push_back(m_Graph.Nodes[i].Id);
					}
					std::string list;
					for (const uint32_t id : ids)
						list += (list.empty() ? "" : ", ") + std::to_string(id);
					AddLinkIssue(link.Index, m_Graph.Nodes[link.To].Id,
						std::format("{} links form a cycle through nodes {}", exec ? "exec" : "data", list), false);
				}
				return cyclic;
			}

			// A required input without a Default needs a link or a value (Incomplete).
			void CheckRequiredInputs()
			{
				for (size_t i = 0; i < m_Graph.Nodes.size(); ++i)
				{
					if (!m_Descriptions[i].has_value())
						continue;
					const GraphNode& node = m_Graph.Nodes[i];
					for (const GraphPinDescription& pin : m_Descriptions[i]->Pins)
					{
						if (pin.Direction != GraphPinDirection::Input || pin.Type == GraphPinType::Exec || pin.Optional || !pin.Default.IsNull())
							continue;
						if (m_LinkedInputs.contains({ i, pin.Name }))
							continue;
						const auto value = node.Values.find(pin.Name);
						if (value != node.Values.end() && !value->second.IsNull())
							continue;
						AddNodeIssue(i, 1, pin.Name, "/Values/" + EscapePointerToken(pin.Name),
							std::format("node {} ({}): input '{}' needs a link{}", node.Id, m_Descriptions[i]->Kind, pin.Name,
								pin.Type == GraphPinType::Entity || pin.Type == GraphPinType::Object ? "" : " or a value"),
							true);
					}
				}
			}

			// A data link from a statement's or an event's output reaches only statements it dominates in the flow from the
			// events (it has run whenever they do), with a virtual root before every event (Incomplete). The exec links form
			// no cycle here, so one pass in topological order gives the immediate dominators.
			void CheckDominance()
			{
				const size_t count = m_Graph.Nodes.size();
				const size_t root = count;
				std::vector<std::vector<size_t>> successors = GetExecSuccessors();
				successors.emplace_back();
				for (size_t i = 0; i < count; ++i)
				{
					if (IsEvent(i))
						successors[root].push_back(i);
				}
				std::vector<std::vector<size_t>> predecessors(count + 1);
				std::vector<size_t> incoming(count + 1, 0);
				std::vector<bool> reached(count + 1, false);
				std::vector<size_t> pending{ root };
				reached[root] = true;
				while (!pending.empty())
				{
					const size_t node = pending.back();
					pending.pop_back();
					for (const size_t next : successors[node])
					{
						predecessors[next].push_back(node);
						++incoming[next];
						if (!reached[next])
						{
							reached[next] = true;
							pending.push_back(next);
						}
					}
				}
				constexpr size_t None = std::numeric_limits<size_t>::max();
				std::vector<size_t> dominator(count + 1, None);
				std::vector<size_t> depth(count + 1, 0);
				dominator[root] = root;
				std::vector<size_t> ready{ root };
				while (!ready.empty())
				{
					const size_t node = ready.back();
					ready.pop_back();
					if (node != root)
					{
						size_t common = None;
						for (const size_t predecessor : predecessors[node])
						{
							if (common == None)
							{
								common = predecessor;
								continue;
							}
							size_t a = common;
							size_t b = predecessor;
							while (a != b)
							{
								if (depth[a] >= depth[b] && a != root)
									a = dominator[a];
								else
									b = dominator[b];
							}
							common = a;
						}
						dominator[node] = common;
						depth[node] = depth[common] + 1;
					}
					for (const size_t next : successors[node])
					{
						if (--incoming[next] == 0)
							ready.push_back(next);
					}
				}
				const auto dominates = [&dominator, &depth, root](size_t candidate, size_t node)
				{
					while (depth[node] > depth[candidate] && node != root)
						node = dominator[node];
					return node == candidate;
				};

				std::vector<std::vector<const Link*>> dataInputs(count);
				// The node each Flow.IfNotNil's True flow enters: its Result is its Value, which is not nil only there. And the node
				// each loop's Body flow enters: its Index and Element are set only there (M24's review), so a loop that ran zero times
				// leaves them nil on its Completed flow and after it.
				std::vector<std::optional<size_t>> trueTargets(count);
				std::vector<std::optional<size_t>> bodyTargets(count);
				for (const Link& link : m_Links)
				{
					if (link.FromPin->Type != GraphPinType::Exec)
						dataInputs[link.To].push_back(&link);
					else if (IsCheckedValue(link.From) && link.FromPin->Name == "True")
						trueTargets[link.From] = link.To;
					else if (IsLoop(link.From) && link.FromPin->Name == "Body")
						bodyTargets[link.From] = link.To;
				}
				// An output that has a value only on one flow (a Flow.IfNotNil's Result on True, a loop's Index and Element on
				// Body), read by `statement`: only where that flow alone enters (decision 6). Its link must be the only exec link
				// into its target (a merge that another flow enters too reads nil on that path), and the target must dominate the
				// statement.
				const auto readsOnFlow = [&predecessors, &dominates](const std::optional<size_t>& target, size_t statement)
				{
					return target.has_value() && predecessors[*target].size() == 1 && dominates(*target, statement);
				};
				std::set<size_t> late;
				std::set<size_t> maybeNil;
				std::set<size_t> loopNil;
				std::vector<size_t> visited(count, None);
				std::vector<size_t> walk;
				for (size_t statement = 0; statement < count; ++statement)
				{
					if (!IsStatement(statement) || !reached[statement] || dominator[statement] == None)
						continue;
					walk.assign(1, statement);
					visited[statement] = statement;
					while (!walk.empty())
					{
						const size_t node = walk.back();
						walk.pop_back();
						for (const Link* link : dataInputs[node])
						{
							if (IsPure(link->From))
							{
								if (visited[link->From] != statement)
								{
									visited[link->From] = statement;
									walk.push_back(link->From);
								}
								continue;
							}
							if (!reached[link->From] || dominator[link->From] == None || !dominates(link->From, statement))
								late.insert(link->Index);
							else if (IsCheckedValue(link->From) && link->FromPin->Name == "Result" && !readsOnFlow(trueTargets[link->From], statement))
								maybeNil.insert(link->Index);
							else if (IsLoop(link->From) && IsLoopValue(*link->FromPin) && NeedsValue(*link->ToPin) && !readsOnFlow(bodyTargets[link->From], statement))
								loopNil.insert(link->Index);
						}
					}
				}
				for (const size_t index : late)
				{
					const GraphLink& link = m_Graph.Links[index];
					AddLinkIssue(index, link.To.Node,
						std::format("output '{}' of node {} is read where node {} may not have run before (only nodes that run after it in its flow, in the same "
									"event, may read it)",
							link.From.Pin, link.From.Node, link.From.Node),
						true);
				}
				for (const size_t index : maybeNil)
				{
					if (late.contains(index))
						continue; // a read where the node may not have run at all is that issue alone
					const GraphLink& link = m_Graph.Links[index];
					AddLinkIssue(index, link.To.Node,
						std::format("output 'Result' of node {} (Flow.IfNotNil) is read where its Value may be nil (only nodes that its True flow alone enters "
									"may read it)",
							link.From.Node),
						true);
				}
				for (const size_t index : loopNil)
				{
					if (late.contains(index))
						continue;
					const GraphLink& link = m_Graph.Links[index];
					const GraphNode* loop = FindGraphNode(m_Graph, link.From.Node);
					AddLinkIssue(index, link.To.Node,
						std::format("output '{}' of node {} ({}) is read where it is nil when the loop runs zero times (only nodes that its Body flow alone "
									"enters may read it into an input that needs a value; elsewhere link it to an optional or Any input)",
							link.From.Pin, link.From.Node, loop != nullptr ? loop->Kind : std::string()),
						true);
				}
			}

			// The function an event or a Return node belongs to (M39): the callback of Event.<Callback> and Return.<Callback>,
			// the method of Method.<Name> and Return.<Name> (a method never takes a callback's name, so the name tells them
			// apart); empty for any other node.
			[[nodiscard]] std::string_view GetFunctionName(size_t node) const
			{
				if (!m_Descriptions[node].has_value())
					return {};
				const std::string_view kind = m_Descriptions[node]->Kind;
				for (const std::string_view prefix : { std::string_view("Event."), GraphMethodKindPrefix, GraphReturnKindPrefix })
				{
					if (kind.starts_with(prefix))
						return kind.substr(prefix.size());
				}
				return {};
			}

			[[nodiscard]] bool IsReturn(size_t node) const
			{
				return m_Descriptions[node].has_value() && m_Descriptions[node]->Kind.starts_with(GraphReturnKindPrefix);
			}

			// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 5): a Return ends the function that runs it, so it is
			// reached only from its own function's event nodes, and never through a Flow.Delay's Later flow, which runs after
			// that function has returned. One walk per function among the events, of the valid exec links, each node reached
			// directly or through a Later flow (the first such Delay kept for the message); only in a graph with a Return.
			void CheckReturns()
			{
				const size_t count = m_Graph.Nodes.size();
				bool anyReturn = false;
				for (size_t node = 0; node < count && !anyReturn; ++node)
					anyReturn = IsReturn(node);
				if (!anyReturn)
					return;
				struct Successor
				{
					size_t Node = 0;
					bool Later = false;
				};
				std::vector<std::vector<Successor>> successors(count);
				for (const Link& link : m_Links)
				{
					if (link.FromPin->Type != GraphPinType::Exec)
						continue;
					const bool later = m_Descriptions[link.From].has_value() && m_Descriptions[link.From]->Kind == "Flow.Delay" && link.FromPin->Name == "Later";
					successors[link.From].push_back({ .Node = link.To, .Later = later });
				}
				// The events of each function, in id order.
				std::map<std::string_view, std::vector<size_t>> functions;
				for (const auto& [id, node] : m_Ids)
				{
					if (IsEvent(node))
						functions[GetFunctionName(node)].push_back(node);
				}
				std::set<size_t> foreign;
				std::set<size_t> late;
				constexpr size_t None = std::numeric_limits<size_t>::max();
				for (const auto& [function, events] : functions)
				{
					// Per node: the event that first reached it directly, and the Delay whose Later flow first reached it.
					std::vector<size_t> origin(count, None);
					std::vector<size_t> delay(count, None);
					std::vector<std::pair<size_t, size_t>> pending; // (node, the Delay it was reached through or None)
					for (const size_t event : events)
					{
						if (origin[event] == None)
						{
							origin[event] = event;
							pending.emplace_back(event, None);
						}
					}
					while (!pending.empty())
					{
						const auto [node, through] = pending.back();
						pending.pop_back();
						for (const Successor& next : successors[node])
						{
							const size_t reachedThrough = through != None ? through : (next.Later ? node : None);
							if (reachedThrough == None)
							{
								if (origin[next.Node] == None)
								{
									origin[next.Node] = origin[node];
									pending.emplace_back(next.Node, None);
								}
							}
							else if (delay[next.Node] == None)
							{
								delay[next.Node] = reachedThrough;
								pending.emplace_back(next.Node, reachedThrough);
							}
						}
					}
					for (size_t node = 0; node < count; ++node)
					{
						if (!IsReturn(node))
							continue;
						const uint32_t id = m_Graph.Nodes[node].Id;
						const std::string& kind = m_Descriptions[node]->Kind;
						const size_t from = origin[node] != None ? origin[node] : (delay[node] != None ? events.front() : None);
						if (from != None && GetFunctionName(node) != function && foreign.insert(node).second)
						{
							AddNodeIssue(node, 0, {}, {},
								std::format("node {} ({}) is reached from node {} ({}), another function's event, but a Return ends only its own function ({})", id, kind,
									m_Graph.Nodes[from].Id, m_Descriptions[from]->Kind, GetFunctionName(node)),
								false);
						}
						if (delay[node] != None && late.insert(node).second)
						{
							AddNodeIssue(node, 0, {}, {},
								std::format("node {} ({}) is reached through the Later flow of node {} (Flow.Delay), which runs after its function has returned, so it "
											"cannot end it; reach it from the Then flow instead",
									id, kind, m_Graph.Nodes[delay[node]].Id),
								false);
						}
					}
				}
			}

			// Whether `node` is a Flow.IfNotNil node, whose Result is its Value on its True flow.
			[[nodiscard]] bool IsCheckedValue(size_t node) const
			{
				return m_Descriptions[node].has_value() && m_Descriptions[node]->Kind == "Flow.IfNotNil";
			}

			// Whether `node` is a loop (Flow.ForRange, Flow.ForEach), whose Index and Element are set on its Body flow alone.
			[[nodiscard]] bool IsLoop(size_t node) const
			{
				return m_Descriptions[node].has_value() && (m_Descriptions[node]->Kind == "Flow.ForRange" || m_Descriptions[node]->Kind == "Flow.ForEach");
			}

			// Whether `pin` is a loop's per-iteration output (Index, Element), not one of its exec outputs.
			[[nodiscard]] static bool IsLoopValue(const GraphPinDescription& pin)
			{
				return pin.Type != GraphPinType::Exec && (pin.Name == "Index" || pin.Name == "Element");
			}

			// Whether an input `pin` needs a value: nil reaches only an optional or an Any input (decision 6's optional rule).
			[[nodiscard]] static bool NeedsValue(const GraphPinDescription& pin)
			{
				return !pin.Optional && pin.Type != GraphPinType::Any;
			}

			const GraphDocument& m_Graph;      // borrowed for the analysis
			const GraphNodeLibrary& m_Library; // borrowed for the analysis
			std::vector<GraphNodeDescription> m_Members{};
			std::vector<std::optional<GraphNodeDescription>> m_Descriptions{};
			std::vector<std::pair<uint32_t, size_t>> m_Ids{}; // (id, index), sorted
			std::vector<Link> m_Links{};
			std::set<std::pair<size_t, std::string>> m_LinkedInputs{}; // (node, pin) of every link's end, valid or not
			std::vector<Issue> m_Issues{};
		};

	}

	std::vector<GraphIssue> ValidateGraph(const GraphDocument& graph, const GraphNodeLibrary& library)
	{
		Utils::GraphAnalysis analysis(graph, library);
		analysis.CheckCyclesAndCompleteness();
		analysis.CheckTextLimits();
		return analysis.TakeIssues();
	}

	std::vector<uint32_t> FindUnreachableGraphNodes(const GraphDocument& graph, const GraphNodeLibrary& library)
	{
		const Utils::GraphAnalysis analysis(graph, library);
		const std::vector<bool> reached = analysis.FindReachable();
		std::vector<uint32_t> unreachable;
		for (size_t i = 0; i < graph.Nodes.size(); ++i)
		{
			if (analysis.IsStatement(i) && !reached[i])
				unreachable.push_back(graph.Nodes[i].Id);
		}
		std::ranges::sort(unreachable);
		return unreachable;
	}

}
