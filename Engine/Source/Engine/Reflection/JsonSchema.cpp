#include "EnginePCH.h"
#include "Engine/Reflection/JsonSchema.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FieldInfo.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Utils {

		static constexpr std::string_view UUIDPattern = "^[0-9a-fA-F]{16}$";
		// An asset reference (§7.1): a handle, which files store, or a project path "Assets/..." (with "#<key>" for a
		// sub-asset) or an engine path "engine://...", which automation params also accept (MethodRegistry.h convention 13).
		static constexpr std::string_view AssetReferencePattern = "^([0-9a-fA-F]{16}|Assets/.+|engine://.+)$";

		static bool IsHexHandle(std::string_view text)
		{
			return text.size() == 16 && std::all_of(text.begin(), text.end(), [](char character)
			{
				return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') || (character >= 'A' && character <= 'F');
			});
		}
		static constexpr std::string_view DefsPrefix = "#/$defs/";
		// $ref hops without descending into the instance before a schema counts as cyclic.
		static constexpr int MaxReferenceHops = 64;

		// A number for a schema keyword: an integer when it is integral and fits int64, a float otherwise.
		static Json MakeSchemaNumber(double value)
		{
			if (std::floor(value) == value && std::fabs(value) < 9.0e18)
				return Json(static_cast<int64_t>(value));
			return Json(value);
		}

		static std::string EscapePointerToken(std::string_view token)
		{
			std::string escaped;
			for (const char character : token)
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

		static std::string UnescapePointerToken(std::string_view token)
		{
			std::string unescaped;
			for (size_t i = 0; i < token.size(); ++i)
			{
				if (token[i] == '~' && i + 1 < token.size() && (token[i + 1] == '0' || token[i + 1] == '1'))
				{
					unescaped += token[i + 1] == '0' ? '~' : '/';
					++i;
				}
				else
				{
					unescaped += token[i];
				}
			}
			return unescaped;
		}

		// Collects the struct definitions a schema references, in the order they are first referenced.
		class SchemaBuilder
		{
		public:
			[[nodiscard]] Json FieldSchema(const FieldInfo& field)
			{
				Json schema = Json::object();
				schema["description"] = field.GetDescription();
				Json body = TypeSchema(field.GetType(), &field.GetMeta());
				for (auto member = body.begin(); member != body.end(); ++member)
					schema[member.key()] = std::move(member.value());
				return schema;
			}

			[[nodiscard]] Json StructDefinition(const StructInfo& type)
			{
				Json definition = Json::object();
				definition["description"] = type.GetDescription();
				definition["type"] = "object";
				Json properties = Json::object();
				for (const Scope<FieldInfo>& field : type.GetFields())
				{
					if (field->IsVirtual() || !field->GetMeta().Serialized)
						continue;
					properties[field->GetName()] = FieldSchema(*field);
				}
				definition["properties"] = std::move(properties);
				definition["additionalProperties"] = false;
				return definition;
			}

			[[nodiscard]] Json Reference(const StructInfo& type)
			{
				if (std::find(m_Referenced.begin(), m_Referenced.end(), &type) == m_Referenced.end())
					m_Referenced.push_back(&type);
				Json reference = Json::object();
				reference["$ref"] = std::string(DefsPrefix) + EscapePointerToken(type.GetName());
				return reference;
			}

			// Adds a definition for `type` now (ForComponents lists every component, referenced or not).
			void Define(const StructInfo& type)
			{
				static_cast<void>(Reference(type));
			}

			// The "$defs" object: every referenced struct, transitively.
			[[nodiscard]] Json BuildDefinitions()
			{
				Json definitions = Json::object();
				for (size_t i = 0; i < m_Referenced.size(); ++i)
				{
					const StructInfo& type = *m_Referenced[i];
					definitions[type.GetName()] = StructDefinition(type);
				}
				return definitions;
			}
		private:
			// Float bounds are rounded to float, as the validators compare them (FieldInfo: a value spelled as the bound is
			// accepted); the canonical writer prints them in their short float spelling.
			[[nodiscard]] static Json NumberSchema(const FieldMeta* meta, double typeMin, double typeMax, bool integer)
			{
				const auto bound = [integer](double value)
				{
					return MakeSchemaNumber(integer ? value : static_cast<double>(static_cast<float>(value)));
				};
				Json schema = Json::object();
				schema["type"] = integer ? "integer" : "number";
				// Plain doubles instead of copied optionals: GCC 14 -O2 reports a copied std::optional<double> as maybe
				// uninitialized here (-Wmaybe-uninitialized false positive).
				const bool hasMinimum = meta != nullptr && meta->Min.has_value();
				const bool hasMaximum = meta != nullptr && meta->Max.has_value();
				const double minimum = hasMinimum ? std::max(*meta->Min, typeMin) : typeMin;
				const double maximum = hasMaximum ? std::min(*meta->Max, typeMax) : typeMax;
				if (integer || hasMinimum)
					schema["minimum"] = bound(minimum);
				if (integer || hasMaximum)
					schema["maximum"] = bound(maximum);
				if (meta != nullptr && meta->MinMagnitude.has_value())
				{
					Json excluded = Json::object();
					excluded["exclusiveMinimum"] = bound(-*meta->MinMagnitude);
					excluded["exclusiveMaximum"] = bound(*meta->MinMagnitude);
					schema["not"] = std::move(excluded);
				}
				return schema;
			}

			[[nodiscard]] static Json TupleSchema(const Json& component, size_t count)
			{
				Json schema = Json::object();
				schema["type"] = "array";
				Json items = Json::array();
				for (size_t i = 0; i < count; ++i)
					items.push_back(component);
				schema["prefixItems"] = std::move(items);
				schema["minItems"] = count;
				schema["maxItems"] = count;
				return schema;
			}

			[[nodiscard]] Json TypeSchema(const TypeInfo& type, const FieldMeta* meta)
			{
				const double floatMax = static_cast<double>(std::numeric_limits<float>::max());
				switch (type.GetKind())
				{
					case FieldType::Bool:
					{
						Json schema = Json::object();
						schema["type"] = "boolean";
						return schema;
					}
					case FieldType::Int32:
						return NumberSchema(meta, static_cast<double>(std::numeric_limits<int32_t>::min()),
							static_cast<double>(std::numeric_limits<int32_t>::max()), true);
					case FieldType::UInt32:
						return NumberSchema(meta, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max()), true);
					case FieldType::Float:
						return NumberSchema(meta, -floatMax, floatMax, false);
					case FieldType::Vec2:
						return TupleSchema(NumberSchema(meta, -floatMax, floatMax, false), 2);
					case FieldType::Vec3:
					case FieldType::Color3:
						return TupleSchema(NumberSchema(meta, -floatMax, floatMax, false), 3);
					case FieldType::Vec4:
					case FieldType::Color4:
						return TupleSchema(NumberSchema(meta, -floatMax, floatMax, false), 4);
					case FieldType::Quat:
						return TupleSchema(NumberSchema(nullptr, -floatMax, floatMax, false), 4);
					case FieldType::Bool3:
					{
						Json component = Json::object();
						component["type"] = "boolean";
						return TupleSchema(component, 3);
					}
					case FieldType::String:
					{
						Json schema = Json::object();
						schema["type"] = "string";
						return schema;
					}
					case FieldType::EntityRef:
					case FieldType::AssetRef:
					{
						Json schema = Json::object();
						Json types = Json::array();
						types.push_back("string");
						types.push_back("null");
						schema["type"] = std::move(types);
						schema["pattern"] = std::string(type.GetKind() == FieldType::AssetRef ? AssetReferencePattern : UUIDPattern);
						const std::string& filter = meta != nullptr ? meta->AssetFilter : type.GetAssetTypeName();
						if (type.GetKind() == FieldType::AssetRef && !filter.empty())
							schema["x-assetType"] = filter;
						return schema;
					}
					case FieldType::Enum:
					{
						Json names = Json::array();
						if (type.GetEnum() != nullptr)
						{
							for (const EnumEntry& entry : type.GetEnum()->GetEntries())
								names.push_back(entry.Name);
						}
						Json schema = Json::object();
						schema["enum"] = std::move(names);
						return schema;
					}
					case FieldType::Array:
					{
						Json schema = Json::object();
						schema["type"] = "array";
						const FieldInfo* elementSchema = type.GetElementSchema();
						schema["items"] = TypeSchema(*type.GetElement(), elementSchema != nullptr ? &elementSchema->GetMeta() : nullptr);
						return schema;
					}
					case FieldType::Struct:
						return Reference(*type.GetStruct());
					case FieldType::Map:
					{
						Json schema = Json::object();
						schema["type"] = "object";
						schema["additionalProperties"] = TypeSchema(*type.GetElement(), nullptr);
						return schema;
					}
					case FieldType::Variant:
						return Json::object();
				}
				return Json::object();
			}
		private:
			std::vector<const StructInfo*> m_Referenced;
		};

		// Validates instances against the subset of JSON Schema 2020-12 the builder emits.
		class SchemaValidator
		{
		public:
			explicit SchemaValidator(const Json& root)
				: m_Root(&root)
			{
			}

			// Violations are collected; the Status fails only for an invalid schema.
			[[nodiscard]] Status Validate(const Json& schema, const Json& instance, int referenceHops)
			{
				if (schema.is_boolean())
				{
					if (schema == false)
						AddViolation("no value is allowed here");
					return {};
				}
				if (!schema.is_object())
					return SchemaError("a schema must be an object or a boolean");

				for (auto keyword = schema.begin(); keyword != schema.end(); ++keyword)
				{
					const std::string& name = keyword.key();
					const Json& argument = keyword.value();
					if (name == "description" || name == "title" || name == "x-assetType" || name == "$schema" || name == "$defs")
						continue;
					if (name == "$ref")
						ENGINE_TRY(ValidateReference(argument, instance, referenceHops));
					else if (name == "type")
						ENGINE_TRY(ValidateType(argument, instance));
					else if (name == "properties" || name == "additionalProperties")
						ENGINE_TRY(ValidateObjectKeyword(name, argument, schema, instance));
					else if (name == "items" || name == "prefixItems")
						ENGINE_TRY(ValidateItems(name, argument, schema, instance));
					else if (name == "minItems" || name == "maxItems")
						ENGINE_TRY(ValidateItemCount(name, argument, instance));
					else if (name == "enum")
						ENGINE_TRY(ValidateEnum(argument, instance));
					else if (name == "minimum" || name == "maximum" || name == "exclusiveMinimum" || name == "exclusiveMaximum")
						ENGINE_TRY(ValidateBound(name, argument, instance));
					else if (name == "not")
						ENGINE_TRY(ValidateNot(argument, instance));
					else if (name == "pattern")
						ENGINE_TRY(ValidatePattern(argument, instance));
					else if (name == "required")
						ENGINE_TRY(ValidateRequired(argument, instance));
					else
						return SchemaError(std::format("unsupported keyword '{}'", name));
				}
				return {};
			}

			[[nodiscard]] std::vector<ErrorIssue> TakeIssues() { return std::move(m_Issues); }
		private:
			[[nodiscard]] Status SchemaError(std::string message) const
			{
				ErrorLocation location;
				location.JsonPointer = m_Pointer;
				return std::unexpected(Error(ErrorCode::InvalidArgument, std::format("invalid schema: {}", message)).WithLocation(std::move(location)));
			}

			void AddViolation(std::string message)
			{
				m_Issues.push_back(ErrorIssue{ m_Pointer, std::move(message), {}, {} });
			}

			[[nodiscard]] static Result<double> ReadNumber(const Json& value)
			{
				return JsonReader(value).ReadDouble();
			}

			[[nodiscard]] Status ValidateChild(const Json& schema, const Json& instance, std::string_view key)
			{
				const size_t length = m_Pointer.size();
				m_Pointer = JsonReader::AppendPointer(m_Pointer, key);
				const Status status = Validate(schema, instance, 0);
				m_Pointer.resize(length);
				return status;
			}

			[[nodiscard]] Status ValidateReference(const Json& argument, const Json& instance, int referenceHops)
			{
				if (!argument.is_string())
					return SchemaError("$ref must be a string");
				ENGINE_TRY_ASSIGN(const std::string reference, JsonReader(argument).ReadString());
				if (!reference.starts_with(DefsPrefix))
					return SchemaError(std::format("unresolvable $ref '{}'", reference));
				if (referenceHops >= MaxReferenceHops)
					return SchemaError(std::format("$ref '{}' is cyclic", reference));

				const auto definitions = m_Root->find("$defs");
				if (definitions == m_Root->end() || !definitions->is_object())
					return SchemaError(std::format("unresolvable $ref '{}': the schema has no $defs", reference));
				const auto target = definitions->find(UnescapePointerToken(std::string_view(reference).substr(DefsPrefix.size())));
				if (target == definitions->end())
					return SchemaError(std::format("unresolvable $ref '{}'", reference));
				return Validate(*target, instance, referenceHops + 1);
			}

			[[nodiscard]] static bool MatchesType(std::string_view type, const Json& instance)
			{
				if (type == "null")
					return instance.is_null();
				if (type == "boolean")
					return instance.is_boolean();
				if (type == "string")
					return instance.is_string();
				if (type == "array")
					return instance.is_array();
				if (type == "object")
					return instance.is_object();
				if (type == "number")
					return instance.is_number();
				if (instance.is_number_integer())
					return true;
				if (!instance.is_number_float())
					return false;
				const Result<double> number = ReadNumber(instance);
				return number.has_value() && std::floor(*number) == *number;
			}

			[[nodiscard]] Status ValidateType(const Json& argument, const Json& instance)
			{
				std::vector<std::string> types;
				if (argument.is_string())
				{
					ENGINE_TRY_ASSIGN(std::string type, JsonReader(argument).ReadString());
					types.push_back(std::move(type));
				}
				else if (argument.is_array())
				{
					for (const Json& element : argument)
					{
						if (!element.is_string())
							return SchemaError("'type' lists a non-string");
						ENGINE_TRY_ASSIGN(std::string type, JsonReader(element).ReadString());
						types.push_back(std::move(type));
					}
				}
				else
				{
					return SchemaError("'type' must be a string or an array of strings");
				}

				for (const std::string& type : types)
				{
					if (type != "null" && type != "boolean" && type != "integer" && type != "number" && type != "string" && type != "array"
						&& type != "object")
					{
						return SchemaError(std::format("unknown type '{}'", type));
					}
					if (MatchesType(type, instance))
						return {};
				}

				std::string expected;
				for (const std::string& type : types)
				{
					if (!expected.empty())
						expected += " or ";
					expected += type;
				}
				AddViolation(std::format("expected {}, got {}", expected, JsonTypeToString(GetJsonType(instance))));
				return {};
			}

			[[nodiscard]] Status ValidateObjectKeyword(const std::string& name, const Json& argument, const Json& schema, const Json& instance)
			{
				if (name == "properties" && !argument.is_object())
					return SchemaError("'properties' must be an object");
				if (name == "additionalProperties" && !argument.is_object() && !argument.is_boolean())
					return SchemaError("'additionalProperties' must be a schema");
				if (!instance.is_object())
					return {};

				const auto properties = schema.find("properties");
				for (auto member = instance.begin(); member != instance.end(); ++member)
				{
					const bool declared = properties != schema.end() && properties->is_object() && properties->contains(member.key());
					if (name == "properties" && declared)
					{
						const auto propertySchema = argument.find(member.key());
						ENGINE_TRY(ValidateChild(*propertySchema, member.value(), member.key()));
					}
					else if (name == "additionalProperties" && !declared)
					{
						if (argument.is_boolean() && argument == false)
							m_Issues.push_back(ErrorIssue{ JsonReader::AppendPointer(m_Pointer, member.key()), std::format("unknown property '{}'", member.key()), {}, {} });
						else
							ENGINE_TRY(ValidateChild(argument, member.value(), member.key()));
					}
				}
				return {};
			}

			// "required" (automation params schemas list their required members, MethodRegistry::GetParamsSchema).
			[[nodiscard]] Status ValidateRequired(const Json& argument, const Json& instance)
			{
				if (!argument.is_array())
					return SchemaError("'required' must be an array of strings");
				for (const Json& name : argument)
				{
					if (!name.is_string())
						return SchemaError("'required' must be an array of strings");
				}
				if (!instance.is_object())
					return {};
				for (const Json& name : argument)
				{
					ENGINE_TRY_ASSIGN(const std::string member, JsonReader(name).ReadString());
					if (!instance.contains(member))
						AddViolation(std::format("missing required property '{}'", member));
				}
				return {};
			}

			[[nodiscard]] Status ValidateItems(const std::string& name, const Json& argument, const Json& schema, const Json& instance)
			{
				if (name == "prefixItems" && !argument.is_array())
					return SchemaError("'prefixItems' must be an array");
				if (name == "items" && !argument.is_object() && !argument.is_boolean())
					return SchemaError("'items' must be a schema");
				if (!instance.is_array())
					return {};

				const auto prefix = schema.find("prefixItems");
				const size_t prefixCount = prefix != schema.end() && prefix->is_array() ? prefix->size() : 0;
				for (size_t i = 0; i < instance.size(); ++i)
				{
					if (name == "prefixItems" && i < argument.size())
						ENGINE_TRY(ValidateChild(argument[i], instance[i], std::to_string(i)));
					else if (name == "items" && i >= prefixCount)
						ENGINE_TRY(ValidateChild(argument, instance[i], std::to_string(i)));
				}
				return {};
			}

			[[nodiscard]] Status ValidateItemCount(const std::string& name, const Json& argument, const Json& instance)
			{
				if (!argument.is_number_integer())
					return SchemaError(std::format("'{}' must be an integer", name));
				ENGINE_TRY_ASSIGN(const uint64_t count, JsonReader(argument).ReadUInt64());
				if (!instance.is_array())
					return {};
				if (name == "minItems" && instance.size() < count)
					AddViolation(std::format("must have at least {} items (got {})", count, instance.size()));
				if (name == "maxItems" && instance.size() > count)
					AddViolation(std::format("must have at most {} items (got {})", count, instance.size()));
				return {};
			}

			[[nodiscard]] Status ValidateEnum(const Json& argument, const Json& instance)
			{
				if (!argument.is_array())
					return SchemaError("'enum' must be an array");
				for (const Json& allowed : argument)
				{
					if (allowed == instance)
						return {};
				}
				const Result<std::string> listed = JsonWriter::Write(argument, JsonStyle::Minified);
				AddViolation(std::format("must be one of {}", listed.has_value() ? *listed : std::string("the listed values")));
				return {};
			}

			[[nodiscard]] Status ValidateBound(const std::string& name, const Json& argument, const Json& instance)
			{
				if (!argument.is_number())
					return SchemaError(std::format("'{}' must be a number", name));
				const Result<double> bound = ReadNumber(argument);
				if (!bound)
					return SchemaError(std::format("'{}' must be a finite number", name));
				if (!instance.is_number())
					return {};
				const Result<double> number = ReadNumber(instance);
				if (!number)
				{
					AddViolation("must be a finite number");
					return {};
				}
				if (name == "minimum" && *number < *bound)
					AddViolation(std::format("must be >= {} (got {})", *bound, *number));
				else if (name == "maximum" && *number > *bound)
					AddViolation(std::format("must be <= {} (got {})", *bound, *number));
				else if (name == "exclusiveMinimum" && *number <= *bound)
					AddViolation(std::format("must be > {} (got {})", *bound, *number));
				else if (name == "exclusiveMaximum" && *number >= *bound)
					AddViolation(std::format("must be < {} (got {})", *bound, *number));
				return {};
			}

			[[nodiscard]] Status ValidateNot(const Json& argument, const Json& instance)
			{
				SchemaValidator inner(*m_Root);
				inner.m_Pointer = m_Pointer;
				ENGINE_TRY(inner.Validate(argument, instance, 0));
				if (inner.m_Issues.empty())
					AddViolation("matches a schema it must not match (a value in an excluded range)");
				return {};
			}

			[[nodiscard]] Status ValidatePattern(const Json& argument, const Json& instance)
			{
				const Result<std::string> pattern = argument.is_string() ? JsonReader(argument).ReadString() : Result<std::string>(std::string());
				const bool isAssetReference = pattern.has_value() && *pattern == AssetReferencePattern;
				if (!pattern.has_value() || (*pattern != UUIDPattern && !isAssetReference))
					return SchemaError(std::format("only the UUID pattern {} and the asset reference pattern {} are supported", UUIDPattern, AssetReferencePattern));
				if (!instance.is_string())
					return {};
				ENGINE_TRY_ASSIGN(const std::string text, JsonReader(instance).ReadString());
				if (!isAssetReference)
				{
					if (!IsHexHandle(text))
						AddViolation("must be a 16-digit hexadecimal UUID string");
					return {};
				}
				const auto isPath = [&text](std::string_view prefix)
				{
					return text.size() > prefix.size() && std::string_view(text).starts_with(prefix);
				};
				if (!IsHexHandle(text) && !isPath("Assets/") && !isPath("engine://"))
					AddViolation("must be a 16-digit hexadecimal handle, a project path \"Assets/...\" or an engine path \"engine://...\"");
				return {};
			}
		private:
			const Json* m_Root = nullptr;
			std::string m_Pointer;
			std::vector<ErrorIssue> m_Issues;
		};

	}

	Json JsonSchema::ForStruct(const StructInfo& type)
	{
		Utils::SchemaBuilder builder;
		Json schema = Json::object();
		schema["$schema"] = std::string(Dialect);
		schema["title"] = type.GetName();
		Json body = builder.StructDefinition(type);
		for (auto member = body.begin(); member != body.end(); ++member)
			schema[member.key()] = std::move(member.value());
		schema["$defs"] = builder.BuildDefinitions();
		return schema;
	}

	Json JsonSchema::ForField(const FieldInfo& field)
	{
		Utils::SchemaBuilder builder;
		Json schema = builder.FieldSchema(field);
		Json definitions = builder.BuildDefinitions();
		if (!definitions.empty())
			schema["$defs"] = std::move(definitions);
		return schema;
	}

	Json JsonSchema::ForComponents(const TypeRegistry& registry)
	{
		Utils::SchemaBuilder builder;
		Json properties = Json::object();
		for (const ComponentInfo* component : registry.GetComponents())
		{
			builder.Define(*component);
			if (component->HasFlag(ComponentFlags::Serializable) && !component->HasFlag(ComponentFlags::EntityLevel))
				properties[component->GetName()] = builder.Reference(*component);
		}

		Json schema = Json::object();
		schema["$schema"] = std::string(Dialect);
		schema["title"] = "Components";
		schema["description"] = "The components of one entity, keyed by registry name (the entity's \"Components\" object).";
		schema["type"] = "object";
		schema["properties"] = std::move(properties);
		schema["additionalProperties"] = false;
		schema["$defs"] = builder.BuildDefinitions();
		return schema;
	}

	Status JsonSchema::Validate(const Json& schema, const Json& instance)
	{
		Utils::SchemaValidator validator(schema);
		ENGINE_TRY(validator.Validate(schema, instance, 0));
		std::vector<ErrorIssue> issues = validator.TakeIssues();
		if (issues.empty())
			return {};

		const bool single = issues.size() == 1;
		std::string message = single ? issues.front().Message : std::format("{} schema violations", issues.size());
		ErrorLocation location;
		location.JsonPointer = single ? issues.front().JsonPointer : std::string();
		return std::unexpected(Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location)).WithIssues(std::move(issues)));
	}

}
