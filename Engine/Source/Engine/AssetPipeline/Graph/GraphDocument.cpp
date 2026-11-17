#include "EnginePCH.h"
#include "Engine/AssetPipeline/Graph/GraphDocument.h"

#include "Engine/Asset/AssetType.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <optional>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

// The graph document (Docs/Decisions/0035-m24-contract.md decision 4). IsGraphIdentifier, FindGraphNode and
// GetNextGraphNodeId are the contract's; the registration, the structural rules, the canonical order and the reader and
// writer are stream G-A's.

namespace Engine {

	namespace Utils {

		// Luau's reserved words, which no graph name may be: the transpiler writes "<Name> = Field..." and "self.<Name>".
		static constexpr std::array<std::string_view, 21> LuauReservedWords = { "and", "break", "do", "else", "elseif", "end", "false", "for",
			"function", "if", "in", "local", "nil", "not", "or", "repeat", "return", "then", "true", "until", "while" };

		// The oldest readable .graph version. An older version migrates in ReadGraphDocument, so every reader sees the current
		// form (decision 10).
		static constexpr uint32_t MinimumGraphVersion = 1;
		// The version that added the methods, the world-pause opt-in and the Any kind (M39,
		// Docs/Decisions/0056-m39-graphs-behaviours.md decision 1).
		static constexpr uint32_t GraphMethodsVersion = 2;
		// The document members version 2 added, which a version 1 file does not have (in member order).
		static constexpr std::array<std::string_view, 2> GraphMethodsVersionMembers = { "RunsWhileWorldPaused", "Methods" };
		// The class key Script.Define reads as a boolean (Scripting/Bindings/Script.cpp), which no method may take.
		static constexpr std::string_view RunsWhileWorldPausedKey = "RunsWhileWorldPaused";
		// The names no method parameter takes: the instance's ("self") and the exec output of its Method.<Name> event nodes,
		// whose data outputs are the parameters.
		static constexpr std::array<std::string_view, 2> ReservedParameterNames = { "self", "Then" };

		// MaxGraphPosition as the whole number its float holds, for messages and descriptions (std::format writes the float 1e+06).
		static constexpr uint32_t MaxGraphPositionInteger = static_cast<uint32_t>(MaxGraphPosition);

		// The names of a field's options, the Luau FieldOptions (GraphField::Options).
		static constexpr std::string_view MinOption = "Min";
		static constexpr std::string_view MaxOption = "Max";
		static constexpr std::string_view StepOption = "Step";
		static constexpr std::string_view TooltipOption = "Tooltip";

		// The registry names of GraphValueKind's values, in value order (RegisterGraphTypes), for messages.
		static constexpr std::array<std::string_view, 12> ValueKindNames = { "Number", "Integer", "Bool", "String", "Vector", "Color", "Quat", "Entity",
			"Asset", "Enum", "Array", "Any" };

		// What the Generate hook gives a value the rules refuse (GenerateGraph).
		static constexpr std::string_view GeneratedGraphName = "Graph";
		static constexpr std::string_view GeneratedMemberName = "Member";
		static constexpr std::string_view GeneratedKind = "Flow.Sequence";
		static constexpr std::string_view GeneratedPin = "Value";
		static constexpr std::string_view GeneratedAssetType = "Texture";
		static constexpr std::string_view GeneratedEnumValue = "Choice";

		[[nodiscard]] static bool IsIdentifierStart(char character)
		{
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
		}

		[[nodiscard]] static bool IsIdentifierCharacter(char character)
		{
			return IsIdentifierStart(character) || (character >= '0' && character <= '9');
		}

		// Whether `name` is a Luau identifier of 1 to MaxGraphNameBytes bytes: [A-Za-z_][A-Za-z0-9_]* and not a reserved word.
		[[nodiscard]] static bool IsLuauName(std::string_view name)
		{
			if (name.empty() || name.size() > MaxGraphNameBytes || !IsIdentifierStart(name.front()))
				return false;
			return std::ranges::all_of(name, &IsIdentifierCharacter) && std::ranges::find(LuauReservedWords, name) == LuauReservedWords.end();
		}

		// A node kind's spelling (the file comment): 1 to MaxGraphKindBytes bytes of dot-separated non-empty parts of
		// [A-Za-z0-9_].
		[[nodiscard]] static bool IsKindSpelling(std::string_view kind)
		{
			if (kind.empty() || kind.size() > MaxGraphKindBytes)
				return false;
			bool partEmpty = true;
			for (const char character : kind)
			{
				if (character == '.')
				{
					if (partEmpty)
						return false;
					partEmpty = true;
					continue;
				}
				if (!IsIdentifierCharacter(character))
					return false;
				partEmpty = false;
			}
			return !partEmpty;
		}

		[[nodiscard]] static bool IsValueKind(GraphValueKind kind)
		{
			return std::to_underlying(kind) <= std::to_underlying(GraphValueKind::Any);
		}

		[[nodiscard]] static std::string_view GetValueKindName(GraphValueKind kind)
		{
			return IsValueKind(kind) ? ValueKindNames[std::to_underlying(kind)] : std::string_view("an unknown kind");
		}

		// Whether a member of `kind` with element `element` names an AssetType (an Asset, or an Array of Asset).
		[[nodiscard]] static bool TakesAssetType(GraphValueKind kind, GraphValueKind element)
		{
			return kind == GraphValueKind::Asset || (kind == GraphValueKind::Array && element == GraphValueKind::Asset);
		}

		// Whether a member of `kind` with element `element` has choices (an Enum, or an Array of Enum).
		[[nodiscard]] static bool TakesEnumValues(GraphValueKind kind, GraphValueKind element)
		{
			return kind == GraphValueKind::Enum || (kind == GraphValueKind::Array && element == GraphValueKind::Enum);
		}

		// Whether the Luau Field constructor of `kind` takes FieldOptions (Field.Entity, Field.Asset, Field.Enum and
		// Field.Array take none, and Any has no Field constructor).
		[[nodiscard]] static bool TakesOptions(GraphValueKind kind)
		{
			return kind != GraphValueKind::Entity && kind != GraphValueKind::Asset && kind != GraphValueKind::Enum && kind != GraphValueKind::Array
				&& kind != GraphValueKind::Any;
		}

		// Whether a field of `kind` takes the numeric options Min, Max and Step.
		[[nodiscard]] static bool TakesBounds(GraphValueKind kind)
		{
			return kind == GraphValueKind::Number || kind == GraphValueKind::Integer || kind == GraphValueKind::Vector;
		}

		// The number `value` (a JSON number) as a double.
		[[nodiscard]] static double ReadNumber(const Json& value)
		{
			return JsonReader(value).ReadDouble().value_or(0.0);
		}

		// The number a .graph file holds for the fractional number `number` (finite, in float's range) once JsonWriter writes
		// it and the reader reads it back: the float it rounds to, in that float's shortest spelling (std::to_chars, as the
		// writer spells it), read by the JSON parser, -0 as 0.
		[[nodiscard]] static double GetFileNumber(double number)
		{
			const auto rounded = static_cast<float>(number);
			if (rounded == 0.0f)
				return 0.0;
			std::array<char, 32> buffer{};
			const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), rounded);
			if (error != std::errc())
				return number; // 32 characters hold every float's shortest spelling
			const Result<Json> parsed = JsonReader::Parse(std::string_view(buffer.data(), static_cast<size_t>(end - buffer.data())));
			return parsed.has_value() ? JsonReader(*parsed).ReadDouble().value_or(number) : number;
		}

		// A problem of `value` as a number of a .graph file, or none. The file holds a number JsonWriter writes back as itself:
		// an integer, or a finite fractional number in float's range that is the float it rounds to (the writer writes every
		// fractional number as that float, -0 as 0), so a file and its canonical re-save hold the same numbers and their
		// transpiled texts are one (decision 7). A number with more digits than its float keeps is refused, naming the
		// float's spelling to write instead.
		[[nodiscard]] static std::optional<std::string> FindNumberProblem(const Json& value)
		{
			if (!value.is_number() || !JsonReader(value).ReadFloat().has_value())
				return std::string("is not a finite number in float's range");
			if (!value.is_number_float())
				return std::nullopt;
			const double number = ReadNumber(value);
			const double held = GetFileNumber(number);
			if (std::bit_cast<uint64_t>(number) == std::bit_cast<uint64_t>(held))
				return std::nullopt;
			return std::format("is {}, which a .graph file holds as the float {}: write {}", number, held, held);
		}

		// The string `value` (a JSON string).
		[[nodiscard]] static std::string ReadText(const Json& value)
		{
			return JsonReader(value).ReadString().value_or(std::string());
		}

		// A problem of the text `text` that must hold 1 (0 when `allowEmpty`) to `maxBytes` bytes of valid UTF-8 without NUL,
		// as the Luau Field constructors require of their names and tooltips, or none.
		[[nodiscard]] static std::optional<std::string> FindTextProblem(std::string_view text, size_t maxBytes, bool allowEmpty)
		{
			if (text.empty() && !allowEmpty)
				return std::string("is empty");
			if (text.size() > maxBytes)
				return std::format("is {} bytes long, more than the {} it may have", text.size(), maxBytes);
			if (!IsValidUtf8(text))
				return std::string("is not valid UTF-8");
			if (text.find('\0') != std::string_view::npos)
				return std::string("holds a NUL character");
			return std::nullopt;
		}

		// A problem of `value` as an array of `count` numbers a .graph file holds (FindNumberProblem), or none.
		[[nodiscard]] static std::optional<std::string> FindNumberArrayProblem(const Json& value, size_t count)
		{
			if (!value.is_array() || value.size() != count)
				return std::format("is not an array of {} numbers", count);
			size_t index = 0;
			for (const Json& element : value)
			{
				if (std::optional<std::string> problem = FindNumberProblem(element))
					return std::format("holds a value at {} that {}", index, *problem);
				++index;
			}
			return std::nullopt;
		}

		// A problem of a node's literal (the file comment: null, a boolean, a finite number, a string of at most
		// MaxGraphStringBytes bytes of valid UTF-8, or an array of 3 or 4 finite numbers), or none.
		[[nodiscard]] static std::optional<std::string> FindLiteralProblem(const Json& value)
		{
			if (value.is_null() || value.is_boolean())
				return std::nullopt;
			if (value.is_number())
				return FindNumberProblem(value);
			if (value.is_string())
			{
				const std::string text = ReadText(value);
				if (text.size() > MaxGraphStringBytes)
					return std::format("is a string of {} bytes, more than the {} a literal may have", text.size(), MaxGraphStringBytes);
				if (!IsValidUtf8(text))
					return std::string("is a string that is not valid UTF-8");
				return std::nullopt;
			}
			if (value.is_array() && (value.size() == 3 || value.size() == 4))
				return FindNumberArrayProblem(value, value.size());
			return std::string("is not null, a boolean, a number, a string or an array of 3 or 4 numbers");
		}

		// A field's Options as the rules read them: the bounds that are valid numbers (the default is checked against them).
		struct GraphFieldBounds
		{
			std::optional<double> Min{};
			std::optional<double> Max{};
		};

		// An Error at `pointer` (absolute).
		[[nodiscard]] static ValidationIssue MakeIssue(std::string pointer, std::string message, std::string hint = {})
		{
			ValidationIssue issue;
			issue.Severity = DiagnosticSeverity::Error;
			issue.JsonPointer = std::move(pointer);
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			return issue;
		}

		// An Error at <context's pointer><relative> (`relative` empty or an already escaped pointer suffix).
		static void AddIssue(ValidationContext& context, std::string_view relative, std::string message, std::string hint = {})
		{
			context.AddIssue(MakeIssue(context.GetPointer() + std::string(relative), std::move(message), std::move(hint)));
		}

		// The issues of a field's Options (`base` the options' absolute pointer) in key order, and the valid bounds they give.
		[[nodiscard]] static std::vector<ValidationIssue> CheckOptions(const std::map<std::string, VariantValue>& options, GraphValueKind kind,
			std::string_view what, const std::string& base, GraphFieldBounds& bounds)
		{
			std::vector<ValidationIssue> issues;
			for (const auto& [key, option] : options)
			{
				const std::string pointer = JsonReader::AppendPointer(base, key);
				if (!TakesOptions(kind))
				{
					issues.push_back(MakeIssue(pointer, std::format("{} is {}, whose Field constructor takes no options, but has the option '{}'", what, GetValueKindName(kind), key),
						"only Number, Integer, Bool, String, Vector, Color and Quat fields have options: remove it"));
					continue;
				}
				const Json& value = option.Get();
				if (key == MinOption || key == MaxOption || key == StepOption)
				{
					if (!TakesBounds(kind))
					{
						issues.push_back(MakeIssue(pointer, std::format("{} is {}, which takes no {} option", what, GetValueKindName(kind), key),
							"Min, Max and Step bound Number, Integer and Vector fields only"));
						continue;
					}
					if (const std::optional<std::string> problem = FindNumberProblem(value))
					{
						issues.push_back(MakeIssue(pointer, std::format("{}'s option {} {}", what, key, *problem)));
						continue;
					}
					const double number = ReadNumber(value);
					if (key == StepOption && !(number > 0.0))
					{
						issues.push_back(MakeIssue(pointer, std::format("{}'s option Step is {}, not a positive number", what, number), "a Step is greater than 0"));
						continue;
					}
					if (key == MinOption)
						bounds.Min = number;
					else if (key == MaxOption)
						bounds.Max = number;
					continue;
				}
				if (key == TooltipOption)
				{
					if (!value.is_string())
						issues.push_back(MakeIssue(pointer, std::format("{}'s Tooltip is not a string", what)));
					else if (const std::optional<std::string> problem = FindTextProblem(ReadText(value), MaxGraphTooltipBytes, true))
						issues.push_back(MakeIssue(pointer, std::format("{}'s Tooltip {}", what, *problem)));
					continue;
				}
				issues.push_back(MakeIssue(pointer, std::format("{} has the unknown option '{}'", what, key), "the options are Min, Max, Step and Tooltip"));
			}
			if (bounds.Min && bounds.Max && *bounds.Min > *bounds.Max)
			{
				issues.push_back(MakeIssue(JsonReader::AppendPointer(base, MaxOption), std::format("{}'s Max {} is below its Min {}", what, *bounds.Max, *bounds.Min),
					"Min <= Max"));
				// Bounds that contradict each other bound nothing: the default is not checked against them, their issue stands.
				bounds = GraphFieldBounds{};
			}
			return issues;
		}

		// A problem of `number`, one component of a Number's, Integer's or Vector's default, against `bounds` as the field's
		// reflected type compares it (a float value against the bound rounded to float, an integer exactly), or none.
		[[nodiscard]] static std::optional<std::string> FindBoundsProblem(double number, GraphValueKind kind, const GraphFieldBounds& bounds)
		{
			const bool floatPrecision = kind != GraphValueKind::Integer;
			const double value = floatPrecision ? static_cast<double>(static_cast<float>(number)) : number;
			const auto comparable = [floatPrecision](double bound)
			{
				return floatPrecision ? static_cast<double>(static_cast<float>(bound)) : bound;
			};
			if (bounds.Min && value < comparable(*bounds.Min))
				return std::format("{} is below the field's Min {}", number, *bounds.Min);
			if (bounds.Max && value > comparable(*bounds.Max))
				return std::format("{} is above the field's Max {}", number, *bounds.Max);
			return std::nullopt;
		}

		// A problem of a member's Default (the file comment's spelling of `kind`), or none. A field's (`bounds` given) Number,
		// Integer or Vector default lies within its bounds, and its Quat default has unit length, as its Luau Field
		// constructor requires of a default; a variable's is any finite nonzero quaternion.
		[[nodiscard]] static std::optional<std::string> FindDefaultProblem(const Json& value, GraphValueKind kind, const std::vector<std::string>& enumValues,
			const GraphFieldBounds* bounds)
		{
			switch (kind)
			{
				case GraphValueKind::Number:
				{
					if (std::optional<std::string> problem = FindNumberProblem(value))
						return problem;
					return bounds != nullptr ? FindBoundsProblem(ReadNumber(value), kind, *bounds) : std::nullopt;
				}
				case GraphValueKind::Integer:
				{
					const Result<int32_t> number = JsonReader(value).ReadInt32();
					if (!number)
						return std::string("is not an integer from -2147483648 to 2147483647");
					return bounds != nullptr ? FindBoundsProblem(static_cast<double>(*number), kind, *bounds) : std::nullopt;
				}
				case GraphValueKind::Bool:
					return value.is_boolean() ? std::nullopt : std::optional<std::string>("is not true or false");
				case GraphValueKind::String:
				{
					if (!value.is_string())
						return std::string("is not a string");
					const std::string text = ReadText(value);
					if (text.size() > MaxGraphStringBytes)
						return std::format("is a string of {} bytes, more than the {} a default may have", text.size(), MaxGraphStringBytes);
					return IsValidUtf8(text) ? std::nullopt : std::optional<std::string>("is not valid UTF-8");
				}
				case GraphValueKind::Vector:
				{
					if (std::optional<std::string> problem = FindNumberArrayProblem(value, 3))
						return problem;
					if (bounds == nullptr)
						return std::nullopt;
					for (const Json& component : value)
					{
						if (std::optional<std::string> problem = FindBoundsProblem(ReadNumber(component), kind, *bounds))
							return problem;
					}
					return std::nullopt;
				}
				case GraphValueKind::Color:
					return FindNumberArrayProblem(value, 4);
				case GraphValueKind::Quat:
				{
					if (std::optional<std::string> problem = FindNumberArrayProblem(value, 4))
						return problem;
					double lengthSquared = 0.0;
					for (const Json& component : value)
					{
						const auto number = static_cast<double>(static_cast<float>(ReadNumber(component)));
						lengthSquared += number * number;
					}
					if (!(lengthSquared > 0.0))
						return std::string("is the zero quaternion");
					const double length = std::sqrt(lengthSquared);
					if (bounds != nullptr && !(std::fabs(length - 1.0) <= UnitQuaternionTolerance))
						return std::format("has length {}, not a unit quaternion's", length);
					return std::nullopt;
				}
				case GraphValueKind::Entity:
				case GraphValueKind::Asset:
				case GraphValueKind::Any:
					return value.is_null() ? std::nullopt : std::optional<std::string>("is not null");
				case GraphValueKind::Enum:
				{
					if (!value.is_string())
						return std::string("is not a string");
					const std::string choice = ReadText(value);
					if (std::ranges::find(enumValues, choice) == enumValues.end())
						return std::format("'{}' is not one of the EnumValues", choice);
					return std::nullopt;
				}
				case GraphValueKind::Array:
					return value.is_array() && value.empty() ? std::nullopt : std::optional<std::string>("is not the empty array []");
			}
			return std::nullopt;
		}

		// The rules of one field's or variable's members after its Name (the file comment), located below `base`, its absolute
		// pointer: Kind, Default, Element, AssetType, EnumValues, then a field's Options (`options` null for a variable).
		static void CheckMember(GraphValueKind kind, const VariantValue& defaultValue, GraphValueKind element, std::string_view assetType,
			const std::vector<std::string>& enumValues, const std::map<std::string, VariantValue>* options, std::string_view what, const std::string& base,
			ValidationContext& context)
		{
			if (!IsValueKind(kind))
			{
				context.AddIssue(MakeIssue(base + "/Kind", std::format("{}'s Kind {} is not a GraphValueKind", what, static_cast<uint32_t>(std::to_underlying(kind)))));
				return;
			}
			// A field is declared by its Luau Field constructor, and no constructor holds any value (M39).
			if (options != nullptr && kind == GraphValueKind::Any)
			{
				context.AddIssue(MakeIssue(base + "/Kind", std::format("{} is of kind Any, which no Field constructor holds", what),
					"a field has a Field constructor's kind; a variable may be Any"));
				return;
			}

			// The options first, so the default is checked against the bounds they give; their issues follow in member order.
			GraphFieldBounds bounds;
			std::vector<ValidationIssue> optionIssues;
			if (options != nullptr)
				optionIssues = CheckOptions(*options, kind, what, base + "/Options", bounds);

			// An Enum without choices has no default to check: the choices' issue stands for it.
			if (kind != GraphValueKind::Enum || !enumValues.empty())
			{
				if (const std::optional<std::string> problem = FindDefaultProblem(defaultValue.Get(), kind, enumValues, options != nullptr ? &bounds : nullptr))
				{
					context.AddIssue(MakeIssue(base + "/Default", std::format("{}'s Default {} (its Kind is {})", what, *problem, GetValueKindName(kind)),
						"a Default has its kind's JSON spelling (reference/FileFormats, Graph)"));
				}
			}

			if (kind == GraphValueKind::Array)
			{
				if (!IsValueKind(element) || element == GraphValueKind::Array || element == GraphValueKind::Any)
					context.AddIssue(MakeIssue(base + "/Element", std::format("{}'s Element is {}, which an Array's element cannot be", what, GetValueKindName(element))));
			}
			else if (element != GraphValueKind::Number)
			{
				context.AddIssue(MakeIssue(base + "/Element", std::format("{}'s Element is {}, but only an Array has an element kind", what, GetValueKindName(element)),
					"Element is Number for every kind but Array"));
			}

			if (TakesAssetType(kind, element))
			{
				const std::optional<AssetType> type = AssetTypeFromString(assetType);
				if (!type || *type == AssetType::None)
				{
					context.AddIssue(MakeIssue(base + "/AssetType", std::format("{}'s AssetType '{}' is not an AssetType", what, assetType),
						"name the AssetType the asset has, such as Texture, AudioClip or Prefab"));
				}
			}
			else if (!assetType.empty())
				context.AddIssue(MakeIssue(base + "/AssetType", std::format("{} names the AssetType '{}', but only an Asset has one", what, assetType)));

			if (TakesEnumValues(kind, element))
			{
				if (enumValues.empty() || enumValues.size() > MaxGraphEnumValues)
				{
					context.AddIssue(MakeIssue(base + "/EnumValues", std::format("{} has {} choices, not 1 to {}", what, enumValues.size(), MaxGraphEnumValues)));
				}
				std::set<std::string_view> seen;
				const size_t count = std::min(enumValues.size(), MaxGraphEnumValues);
				for (size_t index = 0; index < count; ++index)
				{
					const std::string pointer = JsonReader::AppendPointer(base + "/EnumValues", index);
					if (const std::optional<std::string> problem = FindTextProblem(enumValues[index], MaxGraphNameBytes, false))
						context.AddIssue(MakeIssue(pointer, std::format("{}'s choice {} {}", what, index, *problem)));
					else if (!seen.insert(enumValues[index]).second)
						context.AddIssue(MakeIssue(pointer, std::format("{}'s choice '{}' repeats an earlier one", what, enumValues[index]), "each choice appears once"));
				}
			}
			else if (!enumValues.empty())
				context.AddIssue(MakeIssue(base + "/EnumValues", std::format("{} has choices, but only an Enum has them", what)));

			for (ValidationIssue& issue : optionIssues)
				context.AddIssue(std::move(issue));
		}

		// The name rules of one field or variable, located at <base>/Name: a graph identifier that is not a reserved script field
		// name, unique across both lists (`names`, the earlier members' valid names) and after the previous member of its list
		// (`previous`, null for the first).
		static void CheckMemberName(std::string_view name, const std::string* previous, std::string_view what, const std::string& base,
			std::set<std::string_view>& names, ValidationContext& context)
		{
			const std::string pointer = base + "/Name";
			if (!IsGraphIdentifier(name))
			{
				context.AddIssue(MakeIssue(pointer, std::format("{}'s Name '{}' is not a Luau identifier of 1 to {} bytes that is not a reserved word and does not start with \"__\"", what, name, MaxGraphNameBytes), "a member is named like a Luau variable, such as Speed or isOpen"));
				return;
			}
			if (IsReservedScriptFieldName(name))
			{
				context.AddIssue(MakeIssue(pointer, std::format("{}'s Name '{}' is reserved for the behaviour's own members", what, name),
					"Entity, Fields and the callbacks' names (OnStart, OnUpdate, ...) are reserved: rename it"));
				return;
			}
			if (!names.insert(name).second)
			{
				context.AddIssue(MakeIssue(pointer, std::format("{}'s Name '{}' is the name of an earlier field or variable", what, name),
					"a name is unique across the graph's fields and variables"));
				return;
			}
			if (previous != nullptr && name < std::string_view(*previous))
			{
				context.AddIssue(MakeIssue(pointer, std::format("{}'s Name '{}' comes before the previous member's '{}'", what, name, *previous),
					"members are sorted by Name, byte-wise (graph.setMembers keeps them sorted)"));
			}
		}

		// The kind and asset type of a method's parameter or result (M39), located below `base`, its absolute pointer.
		static void CheckMethodValue(GraphValueKind kind, std::string_view assetType, std::string_view what, const std::string& base, ValidationContext& context)
		{
			if (!IsValueKind(kind) || !IsGraphMethodValueKind(kind))
			{
				context.AddIssue(MakeIssue(base + "/Kind", std::format("{}'s Kind is {}, which a method's parameter or result cannot be", what, GetValueKindName(kind)),
					"a parameter or a result is Number, Integer, Bool, String, Vector, Color, Quat, Entity, Asset or Any (a String carries an enum's choice, an Any a "
					"table)"));
			}
			if (kind == GraphValueKind::Asset)
			{
				if (assetType.empty())
					return;
				const std::optional<AssetType> type = AssetTypeFromString(assetType);
				if (!type || *type == AssetType::None)
				{
					context.AddIssue(MakeIssue(base + "/AssetType", std::format("{}'s AssetType '{}' is not an AssetType", what, assetType),
						"name the AssetType the asset has, such as Texture, AudioClip or Prefab, or leave it empty for any asset"));
				}
			}
			else if (!assetType.empty())
				context.AddIssue(MakeIssue(base + "/AssetType", std::format("{} names the AssetType '{}', but only an Asset has one", what, assetType)));
		}

		// The rules of the methods (M39, decision 1), located below `root`, after the fields' and variables' (whose valid
		// names `names` holds, so a method's name is unique across the three lists).
		static void CheckMethods(const GraphDocument& graph, const std::string& root, std::set<std::string_view>& names, ValidationContext& context)
		{
			if (graph.Methods.size() > MaxGraphMethods)
				context.AddIssue(MakeIssue(root + "/Methods", std::format("the graph has {} methods, more than the {} a graph may have", graph.Methods.size(), MaxGraphMethods)));
			const size_t methodCount = std::min(graph.Methods.size(), MaxGraphMethods);
			for (size_t index = 0; index < methodCount; ++index)
			{
				const GraphMethod& method = graph.Methods[index];
				const std::string base = JsonReader::AppendPointer(root + "/Methods", index);
				const std::string what = std::format("method {}", index);
				const std::string name = base + "/Name";
				if (!IsGraphIdentifier(method.Name))
				{
					context.AddIssue(MakeIssue(name, std::format("{}'s Name '{}' is not a Luau identifier of 1 to {} bytes that is not a reserved word and does not start with \"__\"", what, method.Name, MaxGraphNameBytes),
						"a method is named like a Luau function, such as Damaged or Pull"));
				}
				else if (IsReservedScriptFieldName(method.Name) || method.Name == RunsWhileWorldPausedKey)
				{
					context.AddIssue(MakeIssue(name, std::format("{}'s Name '{}' is reserved for the behaviour's own members", what, method.Name),
						"Entity, Fields, RunsWhileWorldPaused and the callbacks' names (OnStart, OnUpdate, ...) are reserved: rename it"));
				}
				else if (!names.insert(method.Name).second)
				{
					context.AddIssue(MakeIssue(name, std::format("{}'s Name '{}' is the name of an earlier field, variable or method", what, method.Name),
						"a name is unique across the graph's fields, variables and methods"));
				}
				else if (index != 0 && method.Name < graph.Methods[index - 1].Name)
				{
					context.AddIssue(MakeIssue(name, std::format("{}'s Name '{}' comes before the previous method's '{}'", what, method.Name, graph.Methods[index - 1].Name),
						"methods are sorted by Name, byte-wise (graph.setMembers keeps them sorted)"));
				}

				if (method.Parameters.size() > MaxGraphMethodParameters)
				{
					context.AddIssue(MakeIssue(base + "/Parameters", std::format("{} has {} parameters, more than the {} a method may have", what, method.Parameters.size(), MaxGraphMethodParameters),
						"Script.Call passes at most four arguments: pass a table of them as one Any parameter"));
				}
				std::set<std::string_view> parameters;
				const size_t parameterCount = std::min(method.Parameters.size(), MaxGraphMethodParameters);
				for (size_t which = 0; which < parameterCount; ++which)
				{
					const GraphParameter& parameter = method.Parameters[which];
					const std::string parameterBase = JsonReader::AppendPointer(base + "/Parameters", which);
					const std::string parameterWhat = std::format("{}'s parameter {}", what, which);
					if (!IsGraphIdentifier(parameter.Name) || std::ranges::find(ReservedParameterNames, parameter.Name) != ReservedParameterNames.end())
					{
						context.AddIssue(MakeIssue(parameterBase + "/Name",
							std::format("{}'s Name '{}' is not a Luau identifier of 1 to {} bytes that is not a reserved word, \"self\", \"Then\" or a name starting "
										"with \"__\"",
								parameterWhat, parameter.Name, MaxGraphNameBytes),
							"a parameter is named like a Luau variable, such as hit or amount (self is the instance, Then its event's exec output)"));
					}
					else if (!parameters.insert(parameter.Name).second)
					{
						context.AddIssue(MakeIssue(parameterBase + "/Name", std::format("{}'s Name '{}' is the name of an earlier parameter of the method", parameterWhat, parameter.Name),
							"a parameter's name is unique in its method"));
					}
					CheckMethodValue(parameter.Kind, parameter.AssetType, parameterWhat, parameterBase, context);
				}

				if (method.Results.size() > MaxGraphMethodResults)
				{
					context.AddIssue(MakeIssue(JsonReader::AppendPointer(base + "/Results", MaxGraphMethodResults),
						std::format("{} has {} results, more than the {} a method may have", what, method.Results.size(), MaxGraphMethodResults),
						"a method returns at most one value: return a table of several as one Any result"));
				}
				const size_t resultCount = std::min(method.Results.size(), MaxGraphMethodResults);
				for (size_t which = 0; which < resultCount; ++which)
				{
					const GraphResult& result = method.Results[which];
					CheckMethodValue(result.Kind, result.AssetType, std::format("{}'s result", what), JsonReader::AppendPointer(base + "/Results", which), context);
				}
			}
		}

		// Every rule of ValidateGraphStructure, located below the context's pointer, in document order. `requireName` false
		// leaves an empty Name unreported: the registry's default object (the "Graph" Validate hook), which a document never is
		// (GraphFromJson runs ValidateGraphStructure, which requires it).
		static void CheckGraph(const GraphDocument& graph, ValidationContext& context, bool requireName)
		{
			const std::string& root = context.GetPointer();
			if ((requireName || !graph.Name.empty()) && !IsGraphIdentifier(graph.Name))
			{
				AddIssue(context, "/Name",
					std::format("the graph's Name '{}' is not a Luau identifier of 1 to {} bytes that is not a reserved word and does not start with \"__\"", graph.Name,
						MaxGraphNameBytes),
					"Name is the behaviour's Script.Define name, such as Door");
			}

			// Fields, then variables: names unique across both lists.
			std::set<std::string_view> names;
			if (graph.Fields.size() > MaxGraphFields)
				AddIssue(context, "/Fields", std::format("the graph has {} fields, more than the {} a graph may have", graph.Fields.size(), MaxGraphFields));
			const size_t fieldCount = std::min(graph.Fields.size(), MaxGraphFields);
			for (size_t index = 0; index < fieldCount; ++index)
			{
				const GraphField& field = graph.Fields[index];
				const std::string base = JsonReader::AppendPointer(root + "/Fields", index);
				const std::string what = std::format("field {}", index);
				CheckMemberName(field.Name, index == 0 ? nullptr : &graph.Fields[index - 1].Name, what, base, names, context);
				CheckMember(field.Kind, field.Default, field.Element, field.AssetType, field.EnumValues, &field.Options, what, base, context);
			}
			if (graph.Variables.size() > MaxGraphVariables)
				AddIssue(context, "/Variables", std::format("the graph has {} variables, more than the {} a graph may have", graph.Variables.size(), MaxGraphVariables));
			const size_t variableCount = std::min(graph.Variables.size(), MaxGraphVariables);
			for (size_t index = 0; index < variableCount; ++index)
			{
				const GraphVariable& variable = graph.Variables[index];
				const std::string base = JsonReader::AppendPointer(root + "/Variables", index);
				const std::string what = std::format("variable {}", index);
				CheckMemberName(variable.Name, index == 0 ? nullptr : &graph.Variables[index - 1].Name, what, base, names, context);
				CheckMember(variable.Kind, variable.Default, variable.Element, variable.AssetType, variable.EnumValues, nullptr, what, base, context);
			}
			CheckMethods(graph, root, names, context);

			// Nodes. The links name nodes by id, so their nodes are checked only while every id is valid and in order (the
			// sorted ids FindGraphNode searches); otherwise the ids' issues stand for them.
			bool idsValid = graph.Nodes.size() <= MaxGraphNodes;
			if (!idsValid)
				AddIssue(context, "/Nodes", std::format("the graph has {} nodes, more than the {} a graph may have", graph.Nodes.size(), MaxGraphNodes));
			std::set<uint32_t> ids;
			const size_t nodeCount = std::min(graph.Nodes.size(), MaxGraphNodes);
			for (size_t index = 0; index < nodeCount; ++index)
			{
				const GraphNode& node = graph.Nodes[index];
				const std::string base = JsonReader::AppendPointer(root + "/Nodes", index);
				if (node.Id == 0 || node.Id > MaxGraphNodeId)
				{
					idsValid = false;
					context.AddIssue(MakeIssue(base + "/Id", std::format("node {}'s Id {} is not from 1 to {}", index, node.Id, MaxGraphNodeId)));
				}
				else if (!ids.insert(node.Id).second)
				{
					idsValid = false;
					context.AddIssue(MakeIssue(base + "/Id", std::format("node {}'s Id {} is the id of an earlier node", index, node.Id), "each node has its own id"));
				}
				else if (index != 0 && node.Id < graph.Nodes[index - 1].Id)
				{
					idsValid = false;
					context.AddIssue(MakeIssue(base + "/Id", std::format("node {}'s Id {} comes before the previous node's {}", index, node.Id, graph.Nodes[index - 1].Id),
						"nodes are sorted by Id"));
				}

				if (!IsKindSpelling(node.Kind))
				{
					context.AddIssue(MakeIssue(base + "/Kind",
						std::format("node {}'s Kind '{}' is not 1 to {} bytes of dot-separated parts of letters, digits and '_'", index, node.Kind, MaxGraphKindBytes),
						"a kind is a node kind of the library, such as Flow.Branch (graph.nodes lists them)"));
				}

				const bool positionValid = std::isfinite(node.Position.x) && std::isfinite(node.Position.y) && std::fabs(node.Position.x) <= MaxGraphPosition
					&& std::fabs(node.Position.y) <= MaxGraphPosition;
				if (!positionValid)
				{
					context.AddIssue(MakeIssue(base + "/Position",
						std::format("node {}'s Position ({}, {}) is not finite within {} of the origin", index, node.Position.x, node.Position.y, Utils::MaxGraphPositionInteger)));
				}

				const std::string values = base + "/Values";
				if (node.Values.size() > MaxGraphNodeValues)
				{
					context.AddIssue(MakeIssue(values, std::format("node {} has {} values, more than the {} a node may have", index, node.Values.size(), MaxGraphNodeValues)));
				}
				size_t checked = 0;
				for (const auto& [pin, value] : node.Values)
				{
					if (checked++ == MaxGraphNodeValues)
						break;
					const std::string pointer = JsonReader::AppendPointer(values, pin);
					if (!IsLuauName(pin))
					{
						context.AddIssue(MakeIssue(pointer, std::format("node {}'s value '{}' does not name a pin (a Luau identifier of 1 to {} bytes)", index, pin, MaxGraphNameBytes),
							"Values are keyed by input pin names, such as Condition (graph.nodes lists each kind's pins)"));
					}
					else if (const std::optional<std::string> problem = FindLiteralProblem(value.Get()))
					{
						context.AddIssue(MakeIssue(pointer, std::format("node {}'s value of '{}' {}", index, pin, *problem),
							"a literal is null, a boolean, a number, a string or an array of 3 or 4 numbers"));
					}
				}
			}

			// Links: each located at its own pointer, one issue each (its first problem).
			if (graph.Links.size() > MaxGraphLinks)
				AddIssue(context, "/Links", std::format("the graph has {} links, more than the {} a graph may have", graph.Links.size(), MaxGraphLinks));
			std::set<std::tuple<uint32_t, std::string_view, uint32_t, std::string_view>> links;
			const size_t linkCount = std::min(graph.Links.size(), MaxGraphLinks);
			for (size_t index = 0; index < linkCount; ++index)
			{
				const GraphLink& link = graph.Links[index];
				const std::string pointer = JsonReader::AppendPointer(root + "/Links", index);
				if (!IsLuauName(link.From.Pin) || !IsLuauName(link.To.Pin))
				{
					context.AddIssue(MakeIssue(pointer,
						std::format("link {} joins the pins '{}' and '{}', which are not both pin names (Luau identifiers of 1 to {} bytes)", index, link.From.Pin, link.To.Pin,
							MaxGraphNameBytes)));
					continue;
				}
				if (idsValid && (FindGraphNode(graph, link.From.Node) == nullptr || FindGraphNode(graph, link.To.Node) == nullptr))
				{
					const uint32_t missing = FindGraphNode(graph, link.From.Node) == nullptr ? link.From.Node : link.To.Node;
					context.AddIssue(MakeIssue(pointer, std::format("link {} names the node {}, which the graph does not hold", index, missing),
						"a link joins two existing nodes: remove it or link another node"));
					continue;
				}
				if (link.From.Node == link.To.Node)
				{
					context.AddIssue(MakeIssue(pointer, std::format("link {} links the node {} to itself", index, link.From.Node), "a link joins two different nodes"));
					continue;
				}
				if (!links.emplace(link.From.Node, link.From.Pin, link.To.Node, link.To.Pin).second)
				{
					context.AddIssue(MakeIssue(pointer, std::format("link {} repeats an earlier link", index), "each link appears once: remove the repeated one"));
					continue;
				}
				if (index != 0)
				{
					const GraphLink& previous = graph.Links[index - 1];
					if (std::tie(link.From, link.To) < std::tie(previous.From, previous.To))
					{
						context.AddIssue(MakeIssue(pointer, std::format("link {} comes before the previous link", index),
							"links are sorted by (From.Node, From.Pin, To.Node, To.Pin), byte-wise"));
					}
				}
			}
		}

		// The "Graph" Validate hook: ValidateGraphStructure's rules, but an empty Name, the registry's default object.
		static void ValidateGraphHook(const GraphDocument& graph, ValidationContext& context)
		{
			CheckGraph(graph, context, false);
		}

		// `name` as a member name that IsGraphIdentifier accepts, is not reserved (for a `method`, IsGraphMethodName) and is
		// not in `taken` (which it joins): itself when it can be, else GeneratedMemberName or the first candidate numbered
		// after it.
		[[nodiscard]] static std::string MakeMemberName(const std::string& name, std::set<std::string>& taken, bool method = false)
		{
			const bool usable = method ? IsGraphMethodName(name) : IsGraphIdentifier(name) && !IsReservedScriptFieldName(name);
			std::string candidate = usable ? name : std::string(GeneratedMemberName);
			const std::string stem = candidate.substr(0, MaxGraphNameBytes - 8);
			for (uint32_t number = 1; taken.contains(candidate); ++number)
				candidate = std::format("{}{}", stem, number);
			taken.insert(candidate);
			return candidate;
		}

		// The default the Luau Field constructor of `kind` gives when it is omitted (Asset/ScriptData.h's ScriptFieldSchema).
		[[nodiscard]] static Json MakeKindDefault(GraphValueKind kind, const std::vector<std::string>& enumValues)
		{
			switch (kind)
			{
				case GraphValueKind::Number:
				case GraphValueKind::Integer: return Json(0);
				case GraphValueKind::Bool:    return Json(false);
				case GraphValueKind::String:  return Json(std::string());
				case GraphValueKind::Vector:  return Json::array({ 0, 0, 0 });
				case GraphValueKind::Color:   return Json::array({ 1, 1, 1, 1 });
				case GraphValueKind::Quat:    return Json::array({ 0, 0, 0, 1 });
				case GraphValueKind::Enum:    return enumValues.empty() ? Json(nullptr) : Json(enumValues.front());
				case GraphValueKind::Array:   return Json::array();
				case GraphValueKind::Entity:
				case GraphValueKind::Asset:
				case GraphValueKind::Any:     return Json(nullptr);
			}
			return Json(nullptr);
		}

		// Repairs one randomized member (GenerateGraph): its element, asset type and choices consistent with its kind, its
		// options (a field's, `options` non-null) those its constructor takes with valid values, and its default valid.
		static void RepairMember(GraphValueKind kind, VariantValue& defaultValue, GraphValueKind& element, std::string& assetType, std::vector<std::string>& enumValues,
			std::map<std::string, VariantValue>* options)
		{
			if (kind != GraphValueKind::Array || element == GraphValueKind::Array || element == GraphValueKind::Any || !IsValueKind(element))
				element = GraphValueKind::Number;
			if (TakesAssetType(kind, element))
			{
				const std::optional<AssetType> type = AssetTypeFromString(assetType);
				if (!type || *type == AssetType::None)
					assetType = GeneratedAssetType;
			}
			else
				assetType.clear();
			if (TakesEnumValues(kind, element))
			{
				std::set<std::string> seen;
				std::erase_if(enumValues, [&seen](const std::string& choice)
				{
					return FindTextProblem(choice, MaxGraphNameBytes, false).has_value() || !seen.insert(choice).second;
				});
				enumValues.resize(std::min(enumValues.size(), MaxGraphEnumValues));
				if (enumValues.empty())
					enumValues.emplace_back(GeneratedEnumValue);
			}
			else
				enumValues.clear();

			GraphFieldBounds bounds;
			if (options != nullptr)
			{
				std::erase_if(*options, [kind](const auto& option)
				{
					GraphFieldBounds ignored;
					return !CheckOptions({ option }, kind, "the field", "", ignored).empty();
				});
				if (!CheckOptions(*options, kind, "the field", "", bounds).empty())
				{
					// Min > Max, the one issue options that pass alone can have: Max goes, and Min bounds the default alone.
					options->erase(std::string(MaxOption));
					bounds = GraphFieldBounds{};
					static_cast<void>(CheckOptions(*options, kind, "the field", "", bounds));
				}
			}
			if (FindDefaultProblem(defaultValue.Get(), kind, enumValues, options != nullptr ? &bounds : nullptr).has_value())
				defaultValue.Set(MakeKindDefault(kind, enumValues));
			// The kind's default outside the bounds: the bounds go.
			if (options != nullptr && FindDefaultProblem(defaultValue.Get(), kind, enumValues, &bounds).has_value())
			{
				options->erase(std::string(MinOption));
				options->erase(std::string(MaxOption));
			}
		}

		// The "Graph" Generate hook: random names become unique valid names, members consistent, nodes numbered from 1 with
		// valid kinds and literals, links between two existing nodes' named pins, and every list in canonical order, so the
		// registry suite's random graphs pass the Validate hook.
		static void GenerateGraph(GraphDocument& graph, Random& /*random*/)
		{
			if (!IsGraphIdentifier(graph.Name))
				graph.Name = GeneratedGraphName;

			std::set<std::string> taken;
			graph.Fields.resize(std::min(graph.Fields.size(), MaxGraphFields));
			for (GraphField& field : graph.Fields)
			{
				field.Name = MakeMemberName(field.Name, taken);
				if (field.Kind == GraphValueKind::Any)
					field.Kind = GraphValueKind::Number;
				RepairMember(field.Kind, field.Default, field.Element, field.AssetType, field.EnumValues, &field.Options);
			}
			graph.Variables.resize(std::min(graph.Variables.size(), MaxGraphVariables));
			for (GraphVariable& variable : graph.Variables)
			{
				variable.Name = MakeMemberName(variable.Name, taken);
				RepairMember(variable.Kind, variable.Default, variable.Element, variable.AssetType, variable.EnumValues, nullptr);
			}
			// Methods (M39): names unique across the members, parameters and results of the kinds and counts a method takes.
			const auto repairValue = [](GraphValueKind& kind, std::string& assetType)
			{
				if (!IsValueKind(kind) || !IsGraphMethodValueKind(kind))
					kind = GraphValueKind::Any;
				if (kind != GraphValueKind::Asset)
					assetType.clear();
				else if (const std::optional<AssetType> type = AssetTypeFromString(assetType); !assetType.empty() && (!type || *type == AssetType::None))
					assetType = GeneratedAssetType;
			};
			graph.Methods.resize(std::min(graph.Methods.size(), MaxGraphMethods));
			for (GraphMethod& method : graph.Methods)
			{
				method.Name = MakeMemberName(method.Name, taken, true);
				method.Parameters.resize(std::min(method.Parameters.size(), MaxGraphMethodParameters));
				std::set<std::string> parameters(ReservedParameterNames.begin(), ReservedParameterNames.end());
				for (GraphParameter& parameter : method.Parameters)
				{
					parameter.Name = MakeMemberName(parameter.Name, parameters);
					repairValue(parameter.Kind, parameter.AssetType);
				}
				method.Results.resize(std::min(method.Results.size(), MaxGraphMethodResults));
				for (GraphResult& result : method.Results)
					repairValue(result.Kind, result.AssetType);
			}

			graph.Nodes.resize(std::min(graph.Nodes.size(), MaxGraphNodes));
			uint32_t id = 0;
			for (GraphNode& node : graph.Nodes)
			{
				node.Id = ++id;
				if (!IsKindSpelling(node.Kind))
					node.Kind = GeneratedKind;
				const auto place = [](float component)
				{
					return std::isfinite(component) ? std::clamp(component, -MaxGraphPosition, MaxGraphPosition) : 0.0f;
				};
				node.Position = glm::vec2(place(node.Position.x), place(node.Position.y));
				std::erase_if(node.Values, [](const auto& value)
				{
					return !IsLuauName(value.first) || FindLiteralProblem(value.second.Get()).has_value();
				});
				while (node.Values.size() > MaxGraphNodeValues)
					node.Values.erase(std::prev(node.Values.end()));
			}

			graph.Links.resize(std::min(graph.Links.size(), MaxGraphLinks));
			const size_t nodeCount = graph.Nodes.size();
			if (nodeCount < 2)
				graph.Links.clear();
			for (GraphLink& link : graph.Links)
			{
				const size_t from = link.From.Node % nodeCount;
				size_t to = link.To.Node % nodeCount;
				if (to == from)
					to = (from + 1) % nodeCount;
				link.From.Node = graph.Nodes[from].Id;
				link.To.Node = graph.Nodes[to].Id;
				if (!IsLuauName(link.From.Pin))
					link.From.Pin = GeneratedPin;
				if (!IsLuauName(link.To.Pin))
					link.To.Pin = GeneratedPin;
			}
			CanonicalizeGraph(graph);
		}

		// The registered "Graph" struct. A registry without it is a programmer error (asserted); the error return keeps builds
		// without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindGraphType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "Graphs need a frozen type registry");
			const StructInfo* type = registry.FindStruct<GraphDocument>();
			ENGINE_CORE_ASSERT(type != nullptr, "Graphs need a registry on which RegisterGraphTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no Graph struct (RegisterGraphTypes was not called)");
			return type;
		}

		[[nodiscard]] static Result<GraphDocument> ReadGraphDocument(const Json& document, const TypeRegistry& registry, GraphLoadReport& report, bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindGraphType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion, root.ReadFormatHeader(GraphFormatName, MinimumGraphVersion, GraphFormatVersion));

			// Everything below the header is the Graph object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			// Version 1 migrates to version 2 (M39) as it reads: its document has no methods and RunsWhileWorldPaused false,
			// so the members version 2 added are unknown in it, warned of and dropped as every unknown member, or with
			// `strictUnknowns` refused.
			if (report.FileVersion < GraphMethodsVersion)
			{
				ValidationContext unknowns;
				for (const std::string_view key : GraphMethodsVersionMembers)
				{
					if (!fields.contains(key))
						continue;
					ValidationIssue issue;
					issue.Severity = strictUnknowns ? DiagnosticSeverity::Error : DiagnosticSeverity::Warning;
					issue.Code = std::string(UnknownFieldCode);
					issue.JsonPointer = JsonReader::AppendPointer("", key);
					issue.Message = std::format("unknown field '{}' on 'Graph': a version {} graph has none (version {} added it)", key, report.FileVersion,
						GraphMethodsVersion);
					issue.Hint = std::format("write \"Version\": {} to declare it", GraphMethodsVersion);
					fields.erase(key);
					if (strictUnknowns)
						unknowns.AddIssue(std::move(issue));
					else
						report.Diagnostics.push_back(std::move(issue));
				}
				ENGINE_TRY(unknowns.ToStatus("the graph"));
			}

			GraphDocument graph;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&graph, JsonReader(fields), context));
			if (report.FileVersion < GraphMethodsVersion)
			{
				// The Any kind is version 2's too.
				ValidationContext kinds;
				for (size_t index = 0; index < graph.Variables.size(); ++index)
				{
					if (graph.Variables[index].Kind != GraphValueKind::Any)
						continue;
					kinds.AddIssue(MakeIssue(JsonReader::AppendPointer("/Variables", index) + "/Kind",
						std::format("variable {} is of kind Any, which a version {} graph does not have (version {} added it)", index, report.FileVersion, GraphMethodsVersion),
						std::format("write \"Version\": {} to declare it", GraphMethodsVersion)));
				}
				ENGINE_TRY(kinds.ToStatus("the graph"));
			}
			ENGINE_TRY(ValidateGraphStructure(graph));
			return graph;
		}

	}

	void RegisterGraphTypes(TypeRegistry& registry)
	{
		// The rules across fields and objects (names, ids, kinds, positions, literals, members, canonical order, limits) are
		// ValidateGraphStructure's, run as the graph's Validate hook; the member structs have none of their own, so a document
		// reports each violation once, in document order.
		registry.Enum<GraphValueKind>("GraphValueKind", "The kind of a graph field, variable or method parameter or result: its Luau Field constructor, or Any.")
			.Entry(GraphValueKind::Number, "Number", "A finite f32 number (Field.Number); Default a number.")
			.Entry(GraphValueKind::Integer, "Integer", "An exact signed 32-bit integer (Field.Integer); Default an integer.")
			.Entry(GraphValueKind::Bool, "Bool", "A boolean (Field.Bool); Default true or false.")
			.Entry(GraphValueKind::String, "String", "A string (Field.String); Default a string.")
			.Entry(GraphValueKind::Vector, "Vector", "A native three-component vector (Field.Vector); Default [x, y, z].")
			.Entry(GraphValueKind::Color, "Color", "A linear RGBA colour (Field.Color); Default [r, g, b, a].")
			.Entry(GraphValueKind::Quat, "Quat", "A quaternion (Field.Quat); Default [x, y, z, w], nonzero, and of unit length for a field.")
			.Entry(GraphValueKind::Entity, "Entity", "An Entity (Field.Entity); Default null.")
			.Entry(GraphValueKind::Asset, "Asset", "An asset reference of one AssetType (Field.Asset); Default null.")
			.Entry(GraphValueKind::Enum, "Enum", "One of EnumValues (Field.Enum); Default one of them.")
			.Entry(GraphValueKind::Array, "Array", "An array of Element values (Field.Array); Default [].")
			.Entry(GraphValueKind::Any, "Any", "Any value, a table included: a variable's (Default null), a method parameter's or a method result's; never a field's.");

		registry.Struct<GraphPinRef>("GraphPinRef", "One pin of one node: an end of a link.")
			.Field("Node", &GraphPinRef::Node, "The node's Id.")
			.Field("Pin", &GraphPinRef::Pin,
				std::format("The pin's name, a Luau identifier of 1 to {} bytes (graph.nodes lists each kind's pins).", MaxGraphNameBytes));

		// The members' descriptions are short: graph.setMembers' MCP input schema inlines both structs (each under 4,096 bytes,
		// §13.8), so their rules are GraphDocument.h's, Architecture §6.11's and FileFormats.md's Graph section's.
		constexpr std::string_view NameRule = "A Luau identifier, unique across fields and variables (§6.11).";
		constexpr std::string_view DefaultRule = "Its default, in its Kind's JSON spelling (§6.11).";
		constexpr std::string_view ElementRule = "An Array's element kind; Number otherwise.";
		constexpr std::string_view AssetTypeRule = "An Asset's AssetType; empty otherwise.";
		constexpr std::string_view EnumValuesRule = "An Enum's choices; empty otherwise.";

		registry.Struct<GraphField>("GraphField", "A behaviour field, set per script slot (§6.11).")
			.Field("Name", &GraphField::Name, NameRule)
			.Field("Kind", &GraphField::Kind, "Its Luau Field constructor; never Any.")
			.Field("Default", &GraphField::Default, DefaultRule)
			.Field("Element", &GraphField::Element, ElementRule)
			.Field("AssetType", &GraphField::AssetType, AssetTypeRule)
			.Field("EnumValues", &GraphField::EnumValues, EnumValuesRule)
			.Field("Options", &GraphField::Options, "Its Luau FieldOptions: Min, Max, Step, Tooltip (§6.11).");

		registry.Struct<GraphVariable>("GraphVariable", "Instance state, set to its Default at OnCreate (§6.11).")
			.Field("Name", &GraphVariable::Name, NameRule)
			.Field("Kind", &GraphVariable::Kind, "Its kind, as a field's, or Any.")
			.Field("Default", &GraphVariable::Default, DefaultRule)
			.Field("Element", &GraphVariable::Element, ElementRule)
			.Field("AssetType", &GraphVariable::AssetType, AssetTypeRule)
			.Field("EnumValues", &GraphVariable::EnumValues, EnumValuesRule);

		// M39 (Docs/Decisions/0056-m39-graphs-behaviours.md decision 1): the methods.
		constexpr std::string_view MethodKindRule = "Its kind: any but Enum and Array.";
		constexpr std::string_view MethodAssetTypeRule = "An Asset's AssetType, or empty for any asset; empty otherwise.";
		registry.Struct<GraphParameter>("GraphParameter", "A parameter of a graph method.")
			.Field("Name", &GraphParameter::Name, "A Luau identifier other than self and Then, unique in its method.")
			.Field("Kind", &GraphParameter::Kind, MethodKindRule)
			.Field("AssetType", &GraphParameter::AssetType, MethodAssetTypeRule);
		registry.Struct<GraphResult>("GraphResult", "The result of a graph method.")
			.Field("Kind", &GraphResult::Kind, MethodKindRule)
			.Field("AssetType", &GraphResult::AssetType, MethodAssetTypeRule);
		registry.Struct<GraphMethod>("GraphMethod",
					"A method of the behaviour, which scripts call on its instance and its Method.<Name> nodes run (§6.11).")
			.Field("Name", &GraphMethod::Name, "A Luau identifier, not a reserved name, unique across fields, variables and methods.")
			.Field("Parameters", &GraphMethod::Parameters, std::format("At most {} parameters, in call order.", MaxGraphMethodParameters))
			.Field("Results", &GraphMethod::Results, std::format("At most {} result; empty when the method returns nothing.", MaxGraphMethodResults));

		registry.Struct<GraphNode>("GraphNode", "One node: a node kind of the library at a place on the canvas, with literals of its input pins.")
			.Field("Id", &GraphNode::Id, std::format("From 1 to {}, unique in the graph and stable for the node's life.", MaxGraphNodeId))
			.Field("Kind", &GraphNode::Kind,
				std::format("Its node kind: 1 to {} bytes of dot-separated parts of letters, digits and '_' (Flow.Branch, Call.Input.GetAxis, "
							"Self.Get.Speed; graph.nodes lists them).",
					MaxGraphKindBytes))
			.Field("Position", &GraphNode::Position,
				std::format("The canvas place of its top-left corner, each component finite and within {} of the origin; layout only.", Utils::MaxGraphPositionInteger))
			.Field("Values", &GraphNode::Values,
				std::format("At most {} literals of its input pins by pin name: null, a boolean, a finite number, a string of at most {} bytes of UTF-8, or an "
							"array of 3 or 4 finite numbers (a vector, a colour, a quaternion). A linked input keeps its literal, and the link wins.",
					MaxGraphNodeValues, MaxGraphStringBytes));

		registry.Struct<GraphLink>("GraphLink", "A link from an output pin of one node to an input pin of another.")
			.Field("From", &GraphLink::From, "The output pin.")
			.Field("To", &GraphLink::To, "The input pin.");

		registry.Struct<GraphDocument>("Graph",
					"A visual-scripting graph (.graph): one behaviour's fields, variables, methods, nodes and links, transpiled to Luau at import.")
			.Field("Name", &GraphDocument::Name,
				std::format("The behaviour's Script.Define name: a Luau identifier of 1 to {} bytes that is not a reserved word and does not start with \"__\".",
					MaxGraphNameBytes))
			.Field("RunsWhileWorldPaused", &GraphDocument::RunsWhileWorldPaused,
				"Whether the behaviour updates while the world is paused (Time.PauseWorld), as a Luau class that sets RunsWhileWorldPaused.")
			.Field("Fields", &GraphDocument::Fields, std::format("At most {} fields, sorted by Name.", MaxGraphFields))
			.Field("Variables", &GraphDocument::Variables, std::format("At most {} variables, sorted by Name.", MaxGraphVariables))
			.Field("Methods", &GraphDocument::Methods, std::format("At most {} methods, sorted by Name.", MaxGraphMethods))
			.Field("Nodes", &GraphDocument::Nodes, std::format("At most {} nodes, sorted by Id.", MaxGraphNodes))
			.Field("Links", &GraphDocument::Links,
				std::format("At most {} links, sorted by (From.Node, From.Pin, To.Node, To.Pin) byte-wise, each once, joining two different nodes.", MaxGraphLinks))
			.Validate(&Utils::ValidateGraphHook)
			.Generate(&Utils::GenerateGraph);
	}

	bool IsGraphIdentifier(std::string_view name)
	{
		if (name.empty() || name.size() > MaxGraphNameBytes || name.starts_with("__"))
			return false;
		const auto isLetter = [](char character)
		{
			return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || character == '_';
		};
		if (!isLetter(name.front()))
			return false;
		return std::ranges::all_of(name, [&isLetter](char character)
		{
			return isLetter(character) || (character >= '0' && character <= '9');
		}) && std::ranges::find(Utils::LuauReservedWords, name) == Utils::LuauReservedWords.end();
	}

	bool IsGraphMethodValueKind(GraphValueKind kind)
	{
		return kind != GraphValueKind::Enum && kind != GraphValueKind::Array;
	}

	bool IsGraphMethodName(std::string_view name)
	{
		return IsGraphIdentifier(name) && !IsReservedScriptFieldName(name) && name != Utils::RunsWhileWorldPausedKey;
	}

	Status ValidateGraphStructure(const GraphDocument& graph)
	{
		ValidationContext context;
		Utils::CheckGraph(graph, context, true);
		return context.ToStatus("the graph");
	}

	void CanonicalizeGraph(GraphDocument& graph)
	{
		const auto byName = [](const auto& member)
		{
			return std::string_view(member.Name);
		};
		std::ranges::stable_sort(graph.Fields, std::ranges::less{}, byName);
		std::ranges::stable_sort(graph.Variables, std::ranges::less{}, byName);
		std::ranges::stable_sort(graph.Methods, std::ranges::less{}, byName);
		std::ranges::stable_sort(graph.Nodes, std::ranges::less{}, &GraphNode::Id);
		std::ranges::stable_sort(graph.Links, [](const GraphLink& left, const GraphLink& right)
		{
			return std::tie(left.From, left.To) < std::tie(right.From, right.To);
		});
		const auto repeated = std::ranges::unique(graph.Links);
		graph.Links.erase(repeated.begin(), repeated.end());
	}

	const GraphNode* FindGraphNode(const GraphDocument& graph, uint32_t id)
	{
		const auto found = std::ranges::lower_bound(graph.Nodes, id, std::less<>(), &GraphNode::Id);
		return found != graph.Nodes.end() && found->Id == id ? &*found : nullptr;
	}

	uint32_t GetNextGraphNodeId(const GraphDocument& graph)
	{
		uint32_t highest = 0;
		for (const GraphNode& node : graph.Nodes)
			highest = std::max(highest, node.Id);
		return highest >= MaxGraphNodeId ? 0 : highest + 1;
	}

	Result<Json> GraphToJson(const GraphDocument& graph, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindGraphType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&graph));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(GraphFormatName);
		document["Version"] = GraphFormatVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> GraphToText(const GraphDocument& graph, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, GraphToJson(graph, registry));
		return JsonWriter::Write(document, style);
	}

	Result<GraphDocument> GraphFromJson(const Json& document, const TypeRegistry& registry, GraphLoadReport& report, bool strictUnknowns)
	{
		report = GraphLoadReport{};
		return Utils::ReadGraphDocument(document, registry, report, strictUnknowns);
	}

	Result<GraphDocument> GraphFromText(std::string_view text, const TypeRegistry& registry, GraphLoadReport& report, bool strictUnknowns)
	{
		report = GraphLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return GraphFromJson(document, registry, report, strictUnknowns);
	}

}
