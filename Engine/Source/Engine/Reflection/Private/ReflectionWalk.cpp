#include "EnginePCH.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/MergePatch.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		static constexpr size_t NoField = std::numeric_limits<size_t>::max();

		static std::string FormatDouble(double value)
		{
			return std::format("{}", value);
		}

		static std::string FormatFloat(float value)
		{
			return std::format("{}", value);
		}

		static std::string_view DescribeValueKind(const Value& value)
		{
			return value.IsNull() ? std::string_view("null") : FieldTypeToString(value.GetKind());
		}

		static ResolveContext WithKey(const ResolveContext& owner, std::string_view key)
		{
			ResolveContext context = owner;
			context.Key = key;
			return context;
		}

		static ResolveContext MakeMemberOwner(const StructInfo& type, const void* object, const JsonReader* json,
			const IFieldSchemaSource* schemas)
		{
			ResolveContext owner;
			owner.Registry = &type.GetRegistry();
			owner.Owner = object;
			owner.OwnerType = &type;
			owner.OwnerJson = json;
			owner.Schemas = schemas;
			return owner;
		}

		// A bound as the stored type sees it: float values compare against the bound rounded to float, so the value a file
		// spells exactly as the bound (Transform.Scale's 0.0001, which rounds below 1e-4) is accepted.
		static double ToComparableBound(double bound, bool floatPrecision)
		{
			return floatPrecision ? static_cast<double>(static_cast<float>(bound)) : bound;
		}

		// Range checks shared by every numeric component (FieldMeta: Min, Max, MinMagnitude; all inclusive).
		static void CheckRange(double value, const FieldMeta* meta, bool floatPrecision, const std::string& text, std::string_view key,
			ValidationContext& validation)
		{
			if (meta == nullptr)
				return;
			if (meta->Min.has_value() && value < ToComparableBound(*meta->Min, floatPrecision))
				validation.Error(key, std::format("must be >= {} (got {})", FormatDouble(*meta->Min), text));
			if (meta->Max.has_value() && value > ToComparableBound(*meta->Max, floatPrecision))
				validation.Error(key, std::format("must be <= {} (got {})", FormatDouble(*meta->Max), text));
			if (meta->MinMagnitude.has_value() && std::fabs(value) < ToComparableBound(*meta->MinMagnitude, floatPrecision))
				validation.Error(key, std::format("must have a magnitude of at least {} (got {})", FormatDouble(*meta->MinMagnitude), text));
		}

		static void CheckFloat(float value, const FieldMeta* meta, std::string_view key, ValidationContext& validation)
		{
			if (!std::isfinite(value))
			{
				validation.Error(key, "must be finite");
				return;
			}
			CheckRange(static_cast<double>(value), meta, true, FormatFloat(value), key, validation);
		}

		static void CheckFloatComponents(std::span<const float> components, const FieldMeta* meta, ValidationContext& validation)
		{
			for (size_t i = 0; i < components.size(); ++i)
				CheckFloat(components[i], meta, std::to_string(i), validation);
		}

		static Result<Json> FloatComponentsToJson(std::span<const float> components)
		{
			Json array = Json::array();
			for (size_t i = 0; i < components.size(); ++i)
			{
				if (!std::isfinite(components[i]))
					return std::unexpected(MakeLocatedValidationError(JsonReader::AppendPointer("", i), "must be finite"));
				array.push_back(static_cast<double>(components[i]));
			}
			return array;
		}

		template<size_t Count>
		static Result<std::array<float, Count>> ReadFloatComponents(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const size_t size, reader.GetArraySize());
			if (size != Count)
			{
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation,
					std::format("expected an array of {} numbers, got {} element(s)", Count, size)));
			}
			std::array<float, Count> components{};
			for (size_t i = 0; i < Count; ++i)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, reader.GetElement(i));
				ENGINE_TRY_ASSIGN(components[i], element.ReadFloat());
			}
			return components;
		}

		static Result<Value> ReadBool3(const JsonReader& reader)
		{
			ENGINE_TRY_ASSIGN(const size_t size, reader.GetArraySize());
			if (size != 3)
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("expected an array of 3 booleans, got {} element(s)", size)));
			glm::bvec3 value(false);
			for (glm::length_t i = 0; i < 3; ++i)
			{
				ENGINE_TRY_ASSIGN(const JsonReader element, reader.GetElement(static_cast<size_t>(i)));
				ENGINE_TRY_ASSIGN(value[i], element.ReadBool());
			}
			return Value::FromBool3(value);
		}

		static Result<Value> ReadEnum(const JsonReader& reader, const TypeInfo& type)
		{
			ENGINE_TRY_ASSIGN(const std::string name, reader.ReadString());
			const EnumInfo* enumInfo = type.GetEnum();
			if (enumInfo == nullptr)
				return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("enum type '{}' has no entries", type.GetName())));
			if (const EnumEntry* entry = enumInfo->FindByName(name))
				return Value::FromEnum(entry->Value);

			const std::string message = std::format("unknown {} value '{}'", enumInfo->GetName(), name);
			std::vector<std::string> suggestions = enumInfo->SuggestNames(name);
			std::string hint = MakeDidYouMeanHint(suggestions);
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, message)
					.WithHint(hint)
					.WithIssue(ErrorIssue{ reader.GetPointer(), message, hint, std::move(suggestions) }));
		}

		static void AddUnknownFieldIssue(const StructInfo& type, std::string_view key, DiagnosticSeverity severity, ValidationContext& validation)
		{
			ValidationIssue issue;
			issue.Severity = severity;
			issue.Code = std::string(UnknownFieldCode);
			issue.JsonPointer = JsonReader::AppendPointer(validation.GetPointer(), key);
			issue.Message = std::format("unknown field '{}' on '{}'", key, type.GetName());
			issue.Suggestions = SuggestSerializedFieldNames(type, key);
			issue.Hint = MakeDidYouMeanHint(issue.Suggestions);
			validation.AddIssue(std::move(issue));
		}

		// The index of the serialized field named `key`, searching from `start` (canonical documents list members in field
		// order, so the next member is usually the next field) and wrapping around.
		static size_t FindSerializedFieldIndex(std::span<const Scope<FieldInfo>> fields, std::string_view key, size_t start)
		{
			for (size_t offset = 0; offset < fields.size(); ++offset)
			{
				const size_t index = (start + offset) % fields.size();
				const FieldInfo& field = *fields[index];
				if (field.GetName() == key && !field.IsVirtual() && field.GetMeta().Serialized)
					return index;
			}
			return NoField;
		}

		size_t ComputeJsonDepth(const Json& value)
		{
			size_t maxDepth = 0;
			std::vector<std::pair<const Json*, size_t>> pending = { { &value, 0 } };
			while (!pending.empty())
			{
				const auto [node, depth] = pending.back();
				pending.pop_back();
				if (!node->is_array() && !node->is_object())
					continue;
				maxDepth = std::max(maxDepth, depth + 1);
				for (const Json& child : *node)
					pending.emplace_back(&child, depth + 1);
			}
			return maxDepth;
		}

		Error MakeLocatedValidationError(std::string pointer, std::string message)
		{
			ErrorLocation location;
			location.JsonPointer = std::move(pointer);
			return Error(ErrorCode::Validation, std::move(message)).WithLocation(std::move(location));
		}

		Error PrependPointer(Error error, std::string_view prefix)
		{
			ErrorLocation location;
			location.JsonPointer = std::string(prefix) + error.GetLocation().JsonPointer.value_or(std::string());
			return std::move(error).WithLocation(std::move(location));
		}

		void AddErrorIssue(const Error& error, ValidationContext& validation)
		{
			ValidationIssue issue;
			issue.Severity = DiagnosticSeverity::Error;
			issue.JsonPointer = validation.GetPointer() + error.GetLocation().JsonPointer.value_or(std::string());
			issue.Message = error.GetMessageText();
			issue.Hint = error.GetHint();
			if (!error.GetIssues().empty())
				issue.Suggestions = error.GetIssues().front().Suggestions;
			validation.AddIssue(std::move(issue));
		}

		bool ContainsVariant(const TypeInfo& type)
		{
			if (type.GetKind() == FieldType::Variant)
				return true;
			if (IsContainerFieldType(type.GetKind()) && type.GetElement() != nullptr)
				return ContainsVariant(*type.GetElement());
			return false;
		}

		std::string DescribeType(const TypeInfo& type)
		{
			if (type.GetKind() == FieldType::Enum || type.GetKind() == FieldType::Struct)
				return type.GetName();
			return std::string(FieldTypeToString(type.GetKind()));
		}

		const FieldInfo* FindSerializedField(const StructInfo& type, std::string_view name)
		{
			const FieldInfo* field = type.FindField(name);
			return field != nullptr && !field->IsVirtual() && field->GetMeta().Serialized ? field : nullptr;
		}

		std::vector<std::string> SuggestSerializedFieldNames(const StructInfo& type, std::string_view name)
		{
			std::vector<std::string_view> names;
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (!field->IsVirtual() && field->GetMeta().Serialized)
					names.push_back(field->GetName());
			}
			return FuzzySuggest(name, names);
		}

		Result<Json> ScalarToJson(const Value& value, const TypeInfo& type)
		{
			const FieldType kind = type.GetKind();
			if (value.IsNull() || value.GetKind() != kind)
				return std::unexpected(MakeLocatedValidationError({}, std::format("expected {}, got {}", DescribeType(type), DescribeValueKind(value))));

			switch (kind)
			{
				case FieldType::Bool:
					return Json(value.AsBool());
				case FieldType::Int32:
					return Json(static_cast<int64_t>(value.AsInt32()));
				case FieldType::UInt32:
					return Json(static_cast<uint64_t>(value.AsUInt32()));
				case FieldType::Float:
				{
					const float number = value.AsFloat();
					if (!std::isfinite(number))
						return std::unexpected(MakeLocatedValidationError({}, "must be finite"));
					return Json(static_cast<double>(number));
				}
				case FieldType::Vec2:
				{
					const glm::vec2 vector = value.AsVec2();
					return FloatComponentsToJson(std::array{ vector.x, vector.y });
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					const glm::vec3 vector = value.AsVec3();
					return FloatComponentsToJson(std::array{ vector.x, vector.y, vector.z });
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					const glm::vec4 vector = value.AsVec4();
					return FloatComponentsToJson(std::array{ vector.x, vector.y, vector.z, vector.w });
				}
				case FieldType::Quat:
				{
					const glm::quat rotation = value.AsQuat();
					return FloatComponentsToJson(std::array{ rotation.x, rotation.y, rotation.z, rotation.w });
				}
				case FieldType::Bool3:
				{
					const glm::bvec3 flags = value.AsBool3();
					Json array = Json::array();
					array.push_back(flags.x);
					array.push_back(flags.y);
					array.push_back(flags.z);
					return array;
				}
				case FieldType::String:
					return Json(value.AsString());
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				{
					const UUID uuid = value.AsUUID();
					return uuid.IsValid() ? Json(uuid.ToString()) : Json(nullptr);
				}
				case FieldType::Enum:
				{
					const EnumInfo* enumInfo = type.GetEnum();
					const EnumEntry* entry = enumInfo != nullptr ? enumInfo->FindByValue(value.AsEnum()) : nullptr;
					if (entry == nullptr)
						return std::unexpected(MakeLocatedValidationError({}, std::format("{} has no enumerator with value {}", type.GetName(), value.AsEnum())));
					return Json(entry->Name);
				}
				case FieldType::Variant:
					return value.AsVariant().Get();
				case FieldType::Array:
				case FieldType::Struct:
				case FieldType::Map:
					break;
			}
			ENGINE_CORE_ASSERT(false, "ScalarToJson called for the composite kind {}", FieldTypeToString(kind));
			return std::unexpected(MakeLocatedValidationError({}, std::format("{} is not a scalar kind", FieldTypeToString(kind))));
		}

		Result<Value> ScalarFromJson(const JsonReader& reader, const TypeInfo& type)
		{
			const FieldType kind = type.GetKind();
			switch (kind)
			{
				case FieldType::Bool:
				{
					ENGINE_TRY_ASSIGN(const bool value, reader.ReadBool());
					return Value::FromBool(value);
				}
				case FieldType::Int32:
				{
					ENGINE_TRY_ASSIGN(const int32_t value, reader.ReadInt32());
					return Value::FromInt32(value);
				}
				case FieldType::UInt32:
				{
					ENGINE_TRY_ASSIGN(const uint32_t value, reader.ReadUInt32());
					return Value::FromUInt32(value);
				}
				case FieldType::Float:
				{
					ENGINE_TRY_ASSIGN(const float value, reader.ReadFloat());
					return Value::FromFloat(value);
				}
				case FieldType::Vec2:
				{
					ENGINE_TRY_ASSIGN(const auto components, ReadFloatComponents<2>(reader));
					return Value::FromVec2(glm::vec2(components[0], components[1]));
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					ENGINE_TRY_ASSIGN(const auto components, ReadFloatComponents<3>(reader));
					const glm::vec3 vector(components[0], components[1], components[2]);
					return kind == FieldType::Vec3 ? Value::FromVec3(vector) : Value::FromColor3(vector);
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					ENGINE_TRY_ASSIGN(const auto components, ReadFloatComponents<4>(reader));
					const glm::vec4 vector(components[0], components[1], components[2], components[3]);
					return kind == FieldType::Vec4 ? Value::FromVec4(vector) : Value::FromColor4(vector);
				}
				case FieldType::Quat:
				{
					// Files spell quaternions [x, y, z, w] (§5.2); glm's constructor takes w first.
					ENGINE_TRY_ASSIGN(const auto components, ReadFloatComponents<4>(reader));
					return Value::FromQuat(glm::quat(components[3], components[0], components[1], components[2]));
				}
				case FieldType::Bool3:
					return ReadBool3(reader);
				case FieldType::String:
				{
					ENGINE_TRY_ASSIGN(std::string value, reader.ReadString());
					return Value::FromString(std::move(value));
				}
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				{
					UUID uuid;
					if (!reader.IsNull())
					{
						ENGINE_TRY_ASSIGN(uuid, reader.ReadUUID());
					}
					return kind == FieldType::EntityRef ? Value::FromEntityRef(uuid) : Value::FromAssetRef(uuid);
				}
				case FieldType::Enum:
					return ReadEnum(reader, type);
				case FieldType::Variant:
					return Value::FromVariant(VariantValue(reader.GetValue()));
				case FieldType::Array:
				case FieldType::Struct:
				case FieldType::Map:
					break;
			}
			ENGINE_CORE_ASSERT(false, "ScalarFromJson called for the composite kind {}", FieldTypeToString(kind));
			return std::unexpected(reader.MakeLocatedError(ErrorCode::Validation, std::format("{} is not a scalar kind", FieldTypeToString(kind))));
		}

		void CheckScalarValue(const Value& value, const TypeInfo& type, const FieldMeta* meta, ValidationContext& validation)
		{
			const FieldType kind = type.GetKind();
			switch (kind)
			{
				case FieldType::Int32:
				{
					const int32_t number = value.AsInt32();
					CheckRange(static_cast<double>(number), meta, false, std::to_string(number), {}, validation);
					return;
				}
				case FieldType::UInt32:
				{
					const uint32_t number = value.AsUInt32();
					CheckRange(static_cast<double>(number), meta, false, std::to_string(number), {}, validation);
					return;
				}
				case FieldType::Float:
					CheckFloat(value.AsFloat(), meta, {}, validation);
					return;
				case FieldType::Vec2:
				{
					const glm::vec2 vector = value.AsVec2();
					CheckFloatComponents(std::array{ vector.x, vector.y }, meta, validation);
					return;
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					const glm::vec3 vector = value.AsVec3();
					CheckFloatComponents(std::array{ vector.x, vector.y, vector.z }, meta, validation);
					return;
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					const glm::vec4 vector = value.AsVec4();
					CheckFloatComponents(std::array{ vector.x, vector.y, vector.z, vector.w }, meta, validation);
					return;
				}
				case FieldType::Quat:
				{
					const glm::quat rotation = value.AsQuat();
					const std::array components = { rotation.x, rotation.y, rotation.z, rotation.w };
					const size_t errors = validation.GetErrorCount();
					CheckFloatComponents(components, nullptr, validation);
					if (validation.GetErrorCount() != errors)
						return;
					double lengthSquared = 0.0;
					for (const float component : components)
						lengthSquared += static_cast<double>(component) * static_cast<double>(component);
					const double length = std::sqrt(lengthSquared);
					if (!(std::fabs(length - 1.0) <= UnitQuaternionTolerance))
						validation.Error({}, std::format("must be a unit quaternion (length {})", FormatDouble(length)));
					return;
				}
				case FieldType::String:
					if (!IsValidUtf8(value.AsString()))
						validation.Error({}, "must be valid UTF-8");
					return;
				case FieldType::Enum:
				{
					const EnumInfo* enumInfo = type.GetEnum();
					if (enumInfo != nullptr && enumInfo->FindByValue(value.AsEnum()) == nullptr)
						validation.Error({}, std::format("{} is not a value of {}", value.AsEnum(), enumInfo->GetName()));
					return;
				}
				case FieldType::Bool:
				case FieldType::Bool3:
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				case FieldType::Variant:
					return;
				case FieldType::Array:
				case FieldType::Struct:
				case FieldType::Map:
					break;
			}
			ENGINE_CORE_ASSERT(false, "CheckScalarValue called for the composite kind {}", FieldTypeToString(kind));
		}

		void CheckWritableJson(const Json& value, ValidationContext& validation)
		{
			const Result<std::string> written = JsonWriter::Write(value, JsonStyle::Minified);
			if (!written)
				AddErrorIssue(written.error(), validation);
		}

		Result<Json> ConvertValueToJson(const Value& value, const TypeInfo& type)
		{
			const FieldType kind = type.GetKind();
			if (IsScalarFieldType(kind))
				return ScalarToJson(value, type);
			if (value.IsNull() || value.GetKind() != kind)
				return std::unexpected(MakeLocatedValidationError({}, std::format("expected {}, got {}", DescribeType(type), DescribeValueKind(value))));

			const std::span<const Value> elements = value.GetElements();
			const std::span<const std::string> keys = value.GetKeys();
			if (kind == FieldType::Array)
			{
				Json array = Json::array();
				for (size_t i = 0; i < elements.size(); ++i)
				{
					Result<Json> element = ConvertValueToJson(elements[i], *type.GetElement());
					if (!element)
						return std::unexpected(PrependPointer(std::move(element).error(), JsonReader::AppendPointer("", i)));
					array.push_back(std::move(*element));
				}
				return array;
			}

			Json object = Json::object();
			const StructInfo* structType = kind == FieldType::Struct ? type.GetStruct() : nullptr;
			for (size_t i = 0; i < elements.size(); ++i)
			{
				const TypeInfo* memberType = type.GetElement();
				if (structType != nullptr)
				{
					const FieldInfo* field = structType->FindField(keys[i]);
					if (field == nullptr || field->IsVirtual())
					{
						return std::unexpected(MakeLocatedValidationError(JsonReader::AppendPointer("", keys[i]),
							std::format("unknown field '{}' on '{}'", keys[i], structType->GetName())));
					}
					if (!field->GetMeta().Serialized)
						continue;
					memberType = &field->GetType();
				}
				Result<Json> member = ConvertValueToJson(elements[i], *memberType);
				if (!member)
					return std::unexpected(PrependPointer(std::move(member).error(), JsonReader::AppendPointer("", keys[i])));
				object[keys[i]] = std::move(*member);
			}
			return object;
		}

		Result<Value> ConvertJsonToValue(const JsonReader& reader, const TypeInfo& type)
		{
			const FieldType kind = type.GetKind();
			if (IsScalarFieldType(kind))
				return ScalarFromJson(reader, type);

			if (kind == FieldType::Array)
			{
				ENGINE_TRY_ASSIGN(const size_t size, reader.GetArraySize());
				std::vector<Value> elements;
				elements.reserve(size);
				for (size_t i = 0; i < size; ++i)
				{
					ENGINE_TRY_ASSIGN(const JsonReader element, reader.GetElement(i));
					ENGINE_TRY_ASSIGN(Value value, ConvertJsonToValue(element, *type.GetElement()));
					elements.push_back(std::move(value));
				}
				return Value::FromArray(std::move(elements));
			}

			ENGINE_TRY(reader.ExpectType(JsonType::Object));
			ENGINE_TRY_ASSIGN(std::vector<std::string> names, reader.GetMemberNames());
			if (kind == FieldType::Map)
			{
				std::sort(names.begin(), names.end());
				std::vector<Value> values;
				values.reserve(names.size());
				for (const std::string& name : names)
				{
					ENGINE_TRY_ASSIGN(const JsonReader member, reader.GetMember(name));
					ENGINE_TRY_ASSIGN(Value value, ConvertJsonToValue(member, *type.GetElement()));
					values.push_back(std::move(value));
				}
				return Value::FromMap(std::move(names), std::move(values));
			}

			const StructInfo& structType = *type.GetStruct();
			for (const std::string& name : names)
			{
				if (FindSerializedField(structType, name) != nullptr)
					continue;
				const std::vector<std::string> suggestions = SuggestSerializedFieldNames(structType, name);
				return std::unexpected(MakeLocatedValidationError(JsonReader::AppendPointer(reader.GetPointer(), name),
					std::format("unknown field '{}' on '{}'", name, structType.GetName()))
						.WithHint(MakeDidYouMeanHint(suggestions)));
			}

			const ObjectPtr defaults = type.HasOps() ? structType.CreateDefault() : ObjectPtr(nullptr, [](void* /*object*/) {});
			std::vector<std::string> fieldNames;
			std::vector<Value> values;
			for (const Scope<FieldInfo>& field : structType.GetFields())
			{
				if (field->IsVirtual())
					continue;
				if (field->GetMeta().Serialized && reader.HasMember(field->GetName()))
				{
					ENGINE_TRY_ASSIGN(const JsonReader member, reader.GetMember(field->GetName()));
					ENGINE_TRY_ASSIGN(Value value, ConvertJsonToValue(member, field->GetType()));
					fieldNames.push_back(field->GetName());
					values.push_back(std::move(value));
				}
				else if (defaults != nullptr && field->IsStored())
				{
					fieldNames.push_back(field->GetName());
					values.push_back(ObjectToValue(field->GetType(), field->GetAddress(static_cast<const void*>(defaults.get()))));
				}
			}
			return Value::FromStruct(std::move(fieldNames), std::move(values));
		}

		Result<Json> ObjectToJson(const TypeInfo& type, const void* object)
		{
			const FieldType kind = type.GetKind();
			const TypeOps& ops = type.GetOps();
			if (IsScalarFieldType(kind))
				return ScalarToJson(ops.Read(object), type);

			if (kind == FieldType::Array)
			{
				Json array = Json::array();
				const size_t count = ops.GetCount(object);
				for (size_t i = 0; i < count; ++i)
				{
					Result<Json> element = ObjectToJson(*type.GetElement(), ops.GetConstElement(object, i));
					if (!element)
						return std::unexpected(PrependPointer(std::move(element).error(), JsonReader::AppendPointer("", i)));
					array.push_back(std::move(*element));
				}
				return array;
			}

			if (kind == FieldType::Map)
			{
				struct MapWrite
				{
					const TypeInfo* Element = nullptr;
					Json Object = Json::object();
					std::optional<Error> Failure;
				};
				MapWrite write;
				write.Element = type.GetElement();
				ops.VisitEntries(object, &write, [](void* context, const std::string& key, const void* value)
				{
					MapWrite& state = *static_cast<MapWrite*>(context);
					if (state.Failure.has_value())
						return;
					Result<Json> element = ObjectToJson(*state.Element, value);
					if (!element)
						state.Failure = PrependPointer(std::move(element).error(), JsonReader::AppendPointer("", key));
					else
						state.Object[key] = std::move(*element);
				});
				if (write.Failure.has_value())
					return std::unexpected(std::move(*write.Failure));
				return std::move(write.Object);
			}

			const StructInfo& structType = *type.GetStruct();
			Json result = Json::object();
			for (const Scope<FieldInfo>& field : structType.GetFields())
			{
				if (!field->IsStored() || !field->GetMeta().Serialized)
					continue;
				Result<Json> member = ObjectToJson(field->GetType(), field->GetAddress(object));
				if (!member)
					return std::unexpected(PrependPointer(std::move(member).error(), JsonReader::AppendPointer("", field->GetName())));
				result[field->GetName()] = std::move(*member);
			}
			return result;
		}

		Value ObjectToValue(const TypeInfo& type, const void* object)
		{
			const FieldType kind = type.GetKind();
			const TypeOps& ops = type.GetOps();
			if (IsScalarFieldType(kind))
				return ops.Read(object);

			if (kind == FieldType::Array)
			{
				std::vector<Value> elements;
				const size_t count = ops.GetCount(object);
				elements.reserve(count);
				for (size_t i = 0; i < count; ++i)
					elements.push_back(ObjectToValue(*type.GetElement(), ops.GetConstElement(object, i)));
				return Value::FromArray(std::move(elements));
			}

			if (kind == FieldType::Map)
			{
				struct MapRead
				{
					const TypeInfo* Element = nullptr;
					std::vector<std::string> Keys;
					std::vector<Value> Values;
				};
				MapRead read;
				read.Element = type.GetElement();
				ops.VisitEntries(object, &read, [](void* context, const std::string& key, const void* value)
				{
					MapRead& state = *static_cast<MapRead*>(context);
					state.Keys.push_back(key);
					state.Values.push_back(ObjectToValue(*state.Element, value));
				});
				return Value::FromMap(std::move(read.Keys), std::move(read.Values));
			}

			std::vector<std::string> names;
			std::vector<Value> values;
			for (const Scope<FieldInfo>& field : type.GetStruct()->GetFields())
			{
				if (!field->IsStored())
					continue;
				names.push_back(field->GetName());
				values.push_back(ObjectToValue(field->GetType(), field->GetAddress(object)));
			}
			return Value::FromStruct(std::move(names), std::move(values));
		}

		void ValueToObject(const TypeInfo& type, const Value& value, void* object)
		{
			const FieldType kind = type.GetKind();
			const TypeOps& ops = type.GetOps();
			if (IsScalarFieldType(kind))
			{
				ops.Write(object, value);
				return;
			}

			const std::span<const Value> elements = value.GetElements();
			const std::span<const std::string> keys = value.GetKeys();
			if (kind == FieldType::Array)
			{
				ops.Resize(object, elements.size());
				for (size_t i = 0; i < elements.size(); ++i)
					ValueToObject(*type.GetElement(), elements[i], ops.GetElement(object, i));
				return;
			}

			if (kind == FieldType::Map)
			{
				ops.Clear(object);
				for (size_t i = 0; i < elements.size(); ++i)
					ValueToObject(*type.GetElement(), elements[i], ops.FindOrInsert(object, keys[i]));
				return;
			}

			ops.Reset(object);
			const StructInfo& structType = *type.GetStruct();
			for (size_t i = 0; i < elements.size(); ++i)
			{
				const FieldInfo* field = structType.FindField(keys[i]);
				if (field != nullptr && field->IsStored())
					ValueToObject(field->GetType(), elements[i], field->GetAddress(object));
			}
		}

		void ReadJson(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const JsonReader& reader, void* object,
			const ResolveContext& owner, WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			const FieldType kind = type.GetKind();
			if (kind == FieldType::Variant)
			{
				CheckVariant(field, reader, owner, walk);
				if (object != nullptr)
					type.GetOps().Write(object, Value::FromVariant(VariantValue(reader.GetValue())));
				return;
			}

			if (IsScalarFieldType(kind))
			{
				const JsonReader leaf(reader.GetValue());
				const Result<Value> value = ScalarFromJson(leaf, type);
				if (!value)
				{
					AddErrorIssue(value.error(), validation);
					return;
				}
				const size_t errors = validation.GetErrorCount();
				CheckScalarValue(*value, type, applyMeta ? &field.GetMeta() : nullptr, validation);
				if (object != nullptr && validation.GetErrorCount() == errors)
					type.GetOps().Write(object, *value);
				return;
			}

			if (kind == FieldType::Struct)
			{
				ReadStructJson(*type.GetStruct(), reader, object, walk);
				return;
			}

			const Json& json = reader.GetValue();
			const TypeInfo& element = *type.GetElement();
			const TypeOps& ops = type.GetOps();
			if (kind == FieldType::Array)
			{
				if (!json.is_array())
				{
					validation.Error({}, std::format("expected array, got {}", JsonTypeToString(reader.GetType())));
					return;
				}
				if (object != nullptr)
					ops.Resize(object, json.size());
				const ResolveContext elementOwner = WithKey(owner, {});
				for (size_t i = 0; i < json.size(); ++i)
				{
					validation.PushKey(std::to_string(i));
					ReadJson(element, field, false, JsonReader(json[i]), object != nullptr ? ops.GetElement(object, i) : nullptr, elementOwner, walk);
					validation.PopKey();
				}
				return;
			}

			if (!json.is_object())
			{
				validation.Error({}, std::format("expected object, got {}", JsonTypeToString(reader.GetType())));
				return;
			}
			if (object != nullptr)
				ops.Clear(object);
			for (auto member = json.begin(); member != json.end(); ++member)
			{
				const std::string& key = member.key();
				validation.PushKey(key);
				ReadJson(element, field, false, JsonReader(member.value()), object != nullptr ? ops.FindOrInsert(object, key) : nullptr,
					WithKey(owner, key), walk);
				validation.PopKey();
			}
		}

		void ReadStructJson(const StructInfo& type, const JsonReader& reader, void* object, WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			const Json& json = reader.GetValue();
			if (!json.is_object())
			{
				validation.Error({}, std::format("expected object, got {}", JsonTypeToString(reader.GetType())));
				return;
			}

			// Validation-only reads of structs with C++ storage go through a scratch object so that the type-level validators,
			// which need one, still run.
			ObjectPtr scratch(nullptr, [](void* /*object*/) {});
			void* target = object;
			if (target == nullptr && type.GetType().HasOps())
			{
				scratch = type.CreateDefault();
				target = scratch.get();
			}

			const size_t errorsBefore = validation.GetErrorCount();
			const std::span<const Scope<FieldInfo>> fields = type.GetFields();
			std::vector<const Json*> members(fields.size(), nullptr);
			size_t searchStart = 0;
			for (auto member = json.begin(); member != json.end(); ++member)
			{
				const size_t index = fields.empty() ? NoField : FindSerializedFieldIndex(fields, member.key(), searchStart);
				if (index == NoField)
				{
					AddUnknownFieldIssue(type, member.key(), DiagnosticSeverity::Warning, validation);
					continue;
				}
				members[index] = &member.value();
				searchStart = index + 1;
			}

			// Fields are read in field order, so a Variant resolver sees every field declared before its own (ResolveContext).
			const JsonReader ownerJson(json);
			const ResolveContext owner = MakeMemberOwner(type, target, &ownerJson, walk.Schemas);
			for (size_t i = 0; i < fields.size(); ++i)
			{
				if (members[i] == nullptr)
					continue;
				const FieldInfo& field = *fields[i];
				validation.PushKey(field.GetName());
				void* address = target != nullptr && field.IsStored() ? field.GetAddress(target) : nullptr;
				ReadJson(field.GetType(), field, true, JsonReader(*members[i]), address, owner, walk);
				validation.PopKey();
			}

			if (target != nullptr && validation.GetErrorCount() == errorsBefore)
				Detail::ReflectionAccess::RunValidators(type, target, validation);
		}

		void ValidateObject(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const void* object, const ResolveContext& owner,
			WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			const FieldType kind = type.GetKind();
			const TypeOps& ops = type.GetOps();
			if (kind == FieldType::Variant)
			{
				const Value value = ops.Read(object);
				CheckVariant(field, JsonReader(value.AsVariant().Get()), owner, walk);
				return;
			}

			if (IsScalarFieldType(kind))
			{
				CheckScalarValue(ops.Read(object), type, applyMeta ? &field.GetMeta() : nullptr, validation);
				return;
			}

			if (kind == FieldType::Struct)
			{
				ValidateStructObject(*type.GetStruct(), object, walk);
				return;
			}

			if (kind == FieldType::Array)
			{
				const ResolveContext elementOwner = WithKey(owner, {});
				const size_t count = ops.GetCount(object);
				for (size_t i = 0; i < count; ++i)
				{
					validation.PushKey(std::to_string(i));
					ValidateObject(*type.GetElement(), field, false, ops.GetConstElement(object, i), elementOwner, walk);
					validation.PopKey();
				}
				return;
			}

			struct MapValidation
			{
				const TypeInfo* Element = nullptr;
				const FieldInfo* Field = nullptr;
				const ResolveContext* Owner = nullptr;
				WalkContext* Walk = nullptr;
			};
			MapValidation state{ type.GetElement(), &field, &owner, &walk };
			ops.VisitEntries(object, &state, [](void* context, const std::string& key, const void* value)
			{
				const MapValidation& map = *static_cast<const MapValidation*>(context);
				ValidationContext& entries = *map.Walk->Validation;
				if (!IsValidUtf8(key))
					entries.Error({}, "map keys must be valid UTF-8");
				entries.PushKey(key);
				ValidateObject(*map.Element, *map.Field, false, value, WithKey(*map.Owner, key), *map.Walk);
				entries.PopKey();
			});
		}

		void ValidateStructObject(const StructInfo& type, const void* object, WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			const size_t errorsBefore = validation.GetErrorCount();
			const ResolveContext owner = MakeMemberOwner(type, object, nullptr, walk.Schemas);
			for (const Scope<FieldInfo>& field : type.GetFields())
			{
				if (!field->IsStored())
					continue;
				validation.PushKey(field->GetName());
				ValidateObject(field->GetType(), *field, true, field->GetAddress(object), owner, walk);
				validation.PopKey();
			}

			// Type-level rules may rely on valid field values (an enum indexing a table), so they run on valid fields only.
			if (validation.GetErrorCount() == errorsBefore)
				Detail::ReflectionAccess::RunValidators(type, object, validation);
		}

		void ValidateValue(const TypeInfo& type, const FieldInfo& field, bool applyMeta, const Value& value, const ResolveContext& owner,
			WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			const FieldType kind = type.GetKind();
			if (value.IsNull() || value.GetKind() != kind)
			{
				validation.Error({}, std::format("expected {}, got {}", DescribeType(type), DescribeValueKind(value)));
				return;
			}

			if (kind == FieldType::Variant)
			{
				CheckVariant(field, JsonReader(value.AsVariant().Get()), owner, walk);
				return;
			}

			if (IsScalarFieldType(kind))
			{
				CheckScalarValue(value, type, applyMeta ? &field.GetMeta() : nullptr, validation);
				return;
			}

			const std::span<const Value> elements = value.GetElements();
			const std::span<const std::string> keys = value.GetKeys();
			if (kind == FieldType::Array)
			{
				const ResolveContext elementOwner = WithKey(owner, {});
				for (size_t i = 0; i < elements.size(); ++i)
				{
					validation.PushKey(std::to_string(i));
					ValidateValue(*type.GetElement(), field, false, elements[i], elementOwner, walk);
					validation.PopKey();
				}
				return;
			}

			if (kind == FieldType::Map)
			{
				for (size_t i = 0; i < elements.size(); ++i)
				{
					if (!IsValidUtf8(keys[i]))
						validation.Error({}, "map keys must be valid UTF-8");
					validation.PushKey(keys[i]);
					ValidateValue(*type.GetElement(), field, false, elements[i], WithKey(owner, keys[i]), walk);
					validation.PopKey();
				}
				return;
			}

			// A struct value has no C++ object, so resolvers of its Variant fields read the preceding fields from its JSON form.
			const StructInfo& structType = *type.GetStruct();
			const size_t errorsBefore = validation.GetErrorCount();
			const Result<Json> json = ConvertValueToJson(value, type);
			std::optional<JsonReader> ownerJson;
			if (json.has_value())
				ownerJson.emplace(*json);
			const ResolveContext memberOwner = MakeMemberOwner(structType, nullptr, ownerJson.has_value() ? &*ownerJson : nullptr, walk.Schemas);
			for (size_t i = 0; i < elements.size(); ++i)
			{
				const FieldInfo* member = structType.FindField(keys[i]);
				if (member == nullptr || member->IsVirtual())
				{
					AddUnknownFieldIssue(structType, keys[i], DiagnosticSeverity::Error, validation);
					continue;
				}
				validation.PushKey(keys[i]);
				ValidateValue(member->GetType(), *member, true, elements[i], memberOwner, walk);
				validation.PopKey();
			}

			if (validation.GetErrorCount() == errorsBefore && structType.HasValidators() && type.HasOps())
			{
				const ObjectPtr scratch = structType.CreateDefault();
				ValueToObject(type, value, scratch.get());
				Detail::ReflectionAccess::RunValidators(structType, scratch.get(), validation);
			}
		}

		void CheckVariant(const FieldInfo& field, const JsonReader& reader, const ResolveContext& resolve, WalkContext& walk)
		{
			ValidationContext& validation = *walk.Validation;
			CheckWritableJson(reader.GetValue(), validation);

			const VariantSchemaResolver resolver = field.GetResolver();
			if (resolver == nullptr)
				return; // free-form JSON

			const Result<const FieldInfo*> schema = resolver(resolve);
			if (!schema.has_value() || *schema == nullptr)
			{
				ValidationIssue issue;
				issue.Severity = DiagnosticSeverity::Warning;
				issue.Code = std::string(VariantUnresolvedCode);
				issue.JsonPointer = validation.GetPointer();
				issue.Message = "its schema cannot be resolved, so it is kept as written";
				if (!schema.has_value())
				{
					issue.Message += ": " + schema.error().GetMessageText();
					issue.Hint = schema.error().GetHint();
				}
				validation.AddIssue(std::move(issue));
				return;
			}

			if (walk.VariantDepth >= MaxVariantNesting)
			{
				validation.Error({}, std::format("nests more than {} Variant values inside each other", MaxVariantNesting));
				return;
			}

			const FieldInfo& resolved = **schema;
			const ResolveContext nested = MakeResolvedSchemaContext(resolved, resolve);
			WalkContext inner = walk;
			++inner.VariantDepth;
			if (walk.Policy == VariantPolicy::Write)
			{
				ReadJson(resolved.GetType(), resolved, true, reader, nullptr, nested, inner);
				return;
			}

			ValidationContext scratch(validation.GetPointer());
			WalkContext scratchWalk = inner;
			scratchWalk.Validation = &scratch;
			ReadJson(resolved.GetType(), resolved, true, reader, nullptr, nested, scratchWalk);
			for (ValidationIssue& issue : scratch.TakeIssues())
			{
				if (issue.Severity == DiagnosticSeverity::Error)
				{
					issue.Severity = DiagnosticSeverity::Warning;
					issue.Code = std::string(VariantSchemaMismatchCode);
					issue.Message = std::format("does not match the schema of '{}', so it is kept as written: {}", resolved.GetName(), issue.Message);
				}
				validation.AddIssue(std::move(issue));
			}
		}

		ResolveContext MakeResolvedSchemaContext(const FieldInfo& schema, const ResolveContext& resolve)
		{
			ResolveContext nested;
			nested.OwnerType = Detail::ReflectionAccess::GetOwner(schema);
			nested.Registry = resolve.Registry != nullptr ? resolve.Registry : (nested.OwnerType != nullptr ? &nested.OwnerType->GetRegistry() : nullptr);
			nested.Schemas = resolve.Schemas;
			return nested;
		}

		Json MergePatchForType(const TypeInfo& type, const Json& target, const Json& patch)
		{
			const FieldType kind = type.GetKind();
			if (kind == FieldType::Variant || !patch.is_object())
				return patch;
			if (kind != FieldType::Struct && kind != FieldType::Map)
				return ApplyMergePatch(target, patch);

			static const Json Absent;
			Json result = target.is_object() ? target : Json::object();
			for (auto member = patch.begin(); member != patch.end(); ++member)
			{
				if (member->is_null())
				{
					result.erase(member.key());
					continue;
				}

				const auto existing = result.find(member.key());
				const Json& previous = existing != result.end() ? *existing : Absent;
				Json merged;
				if (kind == FieldType::Map)
				{
					merged = MergePatchForType(*type.GetElement(), previous, *member);
				}
				else
				{
					const FieldInfo* field = FindSerializedField(*type.GetStruct(), member.key());
					merged = field != nullptr ? MergePatchForType(field->GetType(), previous, *member) : ApplyMergePatch(previous, *member);
				}

				if (existing != result.end())
					*existing = std::move(merged);
				else
					result[member.key()] = std::move(merged);
			}
			return result;
		}

	}

}
