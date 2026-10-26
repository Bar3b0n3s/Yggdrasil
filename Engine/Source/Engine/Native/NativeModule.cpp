#include "EnginePCH.h"
#include "Engine/Native/NativeModule.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/FieldType.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Reflection/JsonSchema.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <format>
#include <limits>
#include <utility>

namespace Engine {

	namespace Utils {

		// The C++ storage of each NativeFieldKind (NativeApi.h): its size, its alignment and, for the float kinds, how many
		// floats it holds.
		struct NativeKindLayout
		{
			uint32_t Size = 0;
			uint32_t Alignment = 0;
			uint32_t FloatCount = 0; // 0 for the kinds without floats
		};

		static constexpr NativeKindLayout GetKindLayout(NativeFieldKind kind)
		{
			switch (kind)
			{
				case NativeFieldKind::Bool:   return { sizeof(bool), alignof(bool), 0 };
				case NativeFieldKind::Int32:  return { sizeof(int32_t), alignof(int32_t), 0 };
				case NativeFieldKind::UInt32: return { sizeof(uint32_t), alignof(uint32_t), 0 };
				case NativeFieldKind::Float:  return { sizeof(float), alignof(float), 1 };
				case NativeFieldKind::Vec2:   return { sizeof(glm::vec2), alignof(glm::vec2), 2 };
				case NativeFieldKind::Vec3:   return { sizeof(glm::vec3), alignof(glm::vec3), 3 };
				case NativeFieldKind::Vec4:   return { sizeof(glm::vec4), alignof(glm::vec4), 4 };
				case NativeFieldKind::Quat:   return { sizeof(glm::quat), alignof(glm::quat), 4 };
				case NativeFieldKind::Color3: return { sizeof(glm::vec3), alignof(glm::vec3), 3 };
				case NativeFieldKind::Color4: return { sizeof(glm::vec4), alignof(glm::vec4), 4 };
				case NativeFieldKind::Entity: return { sizeof(NativeEntity), alignof(NativeEntity), 0 };
				case NativeFieldKind::Enum:   return { sizeof(int32_t), alignof(int32_t), 0 };
			}
			return {};
		}

		static constexpr bool IsKnownKind(NativeFieldKind kind)
		{
			return GetKindLayout(kind).Size != 0;
		}

		static constexpr std::string_view GetKindName(NativeFieldKind kind)
		{
			switch (kind)
			{
				case NativeFieldKind::Bool:   return "Bool";
				case NativeFieldKind::Int32:  return "Int32";
				case NativeFieldKind::UInt32: return "UInt32";
				case NativeFieldKind::Float:  return "Float";
				case NativeFieldKind::Vec2:   return "Vec2";
				case NativeFieldKind::Vec3:   return "Vec3";
				case NativeFieldKind::Vec4:   return "Vec4";
				case NativeFieldKind::Quat:   return "Quat";
				case NativeFieldKind::Color3: return "Color3";
				case NativeFieldKind::Color4: return "Color4";
				case NativeFieldKind::Entity: return "Entity";
				case NativeFieldKind::Enum:   return "Enum";
			}
			return "Unknown";
		}

		// The JSON form a field of `kind` takes, for messages.
		static constexpr std::string_view DescribeKindForm(NativeFieldKind kind)
		{
			switch (kind)
			{
				case NativeFieldKind::Bool:   return "a boolean";
				case NativeFieldKind::Int32:  return "an integer from -2147483648 to 2147483647";
				case NativeFieldKind::UInt32: return "an integer from 0 to 4294967295";
				case NativeFieldKind::Float:  return "a number";
				case NativeFieldKind::Vec2:   return "an array of 2 numbers";
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Color3: return "an array of 3 numbers";
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Color4:
				case NativeFieldKind::Quat:   return "an array of 4 numbers";
				case NativeFieldKind::Entity: return "16 lowercase hex digits or null";
				case NativeFieldKind::Enum:   return "the name of one of its entries";
			}
			return "a value";
		}

		// PascalCase ASCII of at most MaxNativeNameBytes: [A-Z][A-Za-z0-9]*.
		static bool IsValidNativeName(std::string_view name)
		{
			if (name.empty() || name.size() > MaxNativeNameBytes || name.front() < 'A' || name.front() > 'Z')
				return false;
			return std::ranges::all_of(name, [](char character)
			{
				return (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
			});
		}

		// `text` with every byte that starts no well-formed UTF-8 sequence replaced by '?', cut at `maximumBytes` without
		// splitting a code point: a module's exception message may hold anything, and errors reach JSON.
		static std::string SanitizeUtf8(std::string_view text, size_t maximumBytes)
		{
			std::string sanitized;
			size_t offset = 0;
			while (offset < text.size() && sanitized.size() < maximumBytes)
			{
				const size_t length = GetUtf8SequenceLength(text, offset);
				if (length == 0)
				{
					sanitized += '?';
					++offset;
					continue;
				}
				if (sanitized.size() + length > maximumBytes)
					break;
				sanitized.append(text.substr(offset, length));
				offset += length;
			}
			return sanitized;
		}

		// True when both trees are written to the same canonical text (JsonWriter writes every fractional number as a float,
		// so 0.1 and 0.1f are the same text, and 3 and 3.0 too). A tree that cannot be written equals nothing.
		static bool HaveSameText(const Json& left, const Json& right)
		{
			const Result<std::string> leftText = JsonWriter::Write(left, JsonStyle::Minified);
			const Result<std::string> rightText = JsonWriter::Write(right, JsonStyle::Minified);
			return leftText.has_value() && rightText.has_value() && *leftText == *rightText;
		}

		template<typename T>
		static T LoadMember(std::span<const std::byte> value, uint32_t offset)
		{
			T member{};
			std::memcpy(&member, value.data() + offset, sizeof(T));
			return member;
		}

		template<typename T>
		static void StoreMember(std::span<std::byte> value, uint32_t offset, const T& member)
		{
			std::memcpy(value.data() + offset, &member, sizeof(T));
		}

		// The floats of a float field (Float, vectors, colours, quaternions as x, y, z, w).
		static std::array<float, 4> LoadFloats(const NativeFieldInfo& field, std::span<const std::byte> value)
		{
			switch (field.Kind)
			{
				case NativeFieldKind::Float:
					return { LoadMember<float>(value, field.Offset), 0.0f, 0.0f, 0.0f };
				case NativeFieldKind::Vec2:
				{
					const auto vector = LoadMember<glm::vec2>(value, field.Offset);
					return { vector.x, vector.y, 0.0f, 0.0f };
				}
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Color3:
				{
					const auto vector = LoadMember<glm::vec3>(value, field.Offset);
					return { vector.x, vector.y, vector.z, 0.0f };
				}
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Color4:
				{
					const auto vector = LoadMember<glm::vec4>(value, field.Offset);
					return { vector.x, vector.y, vector.z, vector.w };
				}
				case NativeFieldKind::Quat:
				{
					const auto rotation = LoadMember<glm::quat>(value, field.Offset);
					return { rotation.x, rotation.y, rotation.z, rotation.w };
				}
				case NativeFieldKind::Bool:
				case NativeFieldKind::Int32:
				case NativeFieldKind::UInt32:
				case NativeFieldKind::Entity:
				case NativeFieldKind::Enum:
					break;
			}
			return {};
		}

		static void StoreFloats(const NativeFieldInfo& field, std::span<std::byte> value, const std::array<float, 4>& floats)
		{
			switch (field.Kind)
			{
				case NativeFieldKind::Float:
					StoreMember(value, field.Offset, floats[0]);
					return;
				case NativeFieldKind::Vec2:
					StoreMember(value, field.Offset, glm::vec2(floats[0], floats[1]));
					return;
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Color3:
					StoreMember(value, field.Offset, glm::vec3(floats[0], floats[1], floats[2]));
					return;
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Color4:
					StoreMember(value, field.Offset, glm::vec4(floats[0], floats[1], floats[2], floats[3]));
					return;
				case NativeFieldKind::Quat:
					// Files spell quaternions [x, y, z, w]; glm's constructor takes w first.
					StoreMember(value, field.Offset, glm::quat(floats[3], floats[0], floats[1], floats[2]));
					return;
				case NativeFieldKind::Bool:
				case NativeFieldKind::Int32:
				case NativeFieldKind::UInt32:
				case NativeFieldKind::Entity:
				case NativeFieldKind::Enum:
					return;
			}
		}

		// A float bound as the stored float sees it, so a value spelled exactly as the bound is accepted (as FieldInfo does).
		static double ToFloatBound(double bound)
		{
			return static_cast<double>(static_cast<float>(bound));
		}

		static std::string FormatNumber(double value)
		{
			return std::format("{}", value);
		}

		// Appends the issues of `number` against the field's bounds; `floatPrecision` compares with the bounds rounded to float.
		static void CheckBounds(const NativeFieldInfo& field, double number, bool floatPrecision, const std::string& pointer, std::vector<ErrorIssue>& issues)
		{
			if (field.Min.has_value() && number < (floatPrecision ? ToFloatBound(*field.Min) : *field.Min))
				issues.push_back({ .JsonPointer = pointer, .Message = std::format("must be >= {} (got {})", FormatNumber(*field.Min), FormatNumber(number)), .Hint = {}, .Suggestions = {} });
			if (field.Max.has_value() && number > (floatPrecision ? ToFloatBound(*field.Max) : *field.Max))
				issues.push_back({ .JsonPointer = pointer, .Message = std::format("must be <= {} (got {})", FormatNumber(*field.Max), FormatNumber(number)), .Hint = {}, .Suggestions = {} });
		}

		static const NativeEnumValue* FindEnumValue(const NativeFieldInfo& field, int64_t value)
		{
			const auto found = std::ranges::find(field.EnumValues, value, [](const NativeEnumValue& entry)
			{
				return static_cast<int64_t>(entry.Value);
			});
			return found == field.EnumValues.end() ? nullptr : &*found;
		}

		static const NativeEnumValue* FindEnumName(const NativeFieldInfo& field, std::string_view name)
		{
			const auto found = std::ranges::find(field.EnumValues, name, &NativeEnumValue::Name);
			return found == field.EnumValues.end() ? nullptr : &*found;
		}

		// The issues of the field's stored value: finite numbers within the bounds, unit quaternions, enum values that name an
		// entry. Booleans, integers and entities hold no invalid value of their kind but those the bounds exclude.
		static void CheckField(const NativeFieldInfo& field, std::span<const std::byte> value, const std::string& pointer, std::vector<ErrorIssue>& issues)
		{
			switch (field.Kind)
			{
				case NativeFieldKind::Bool:
				case NativeFieldKind::Entity:
					return;
				case NativeFieldKind::Int32:
					CheckBounds(field, static_cast<double>(LoadMember<int32_t>(value, field.Offset)), false, pointer, issues);
					return;
				case NativeFieldKind::UInt32:
					CheckBounds(field, static_cast<double>(LoadMember<uint32_t>(value, field.Offset)), false, pointer, issues);
					return;
				case NativeFieldKind::Enum:
				{
					const int32_t number = LoadMember<int32_t>(value, field.Offset);
					if (FindEnumValue(field, number) == nullptr)
						issues.push_back({ .JsonPointer = pointer, .Message = std::format("{} is not the value of an entry of '{}'", number, field.Name), .Hint = {}, .Suggestions = {} });
					return;
				}
				case NativeFieldKind::Float:
				case NativeFieldKind::Vec2:
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Quat:
				case NativeFieldKind::Color3:
				case NativeFieldKind::Color4:
					break;
			}

			const uint32_t count = GetKindLayout(field.Kind).FloatCount;
			const std::array<float, 4> floats = LoadFloats(field, value);
			const size_t issueCount = issues.size();
			for (uint32_t index = 0; index < count; ++index)
			{
				const std::string componentPointer = count == 1 ? pointer : JsonReader::AppendPointer(pointer, index);
				if (!std::isfinite(floats[index]))
				{
					issues.push_back({ .JsonPointer = componentPointer, .Message = "must be finite", .Hint = {}, .Suggestions = {} });
					continue;
				}
				CheckBounds(field, static_cast<double>(floats[index]), true, componentPointer, issues);
			}
			if (field.Kind == NativeFieldKind::Quat && issues.size() == issueCount)
			{
				double lengthSquared = 0.0;
				for (const float component : floats)
					lengthSquared += static_cast<double>(component) * static_cast<double>(component);
				const double length = std::sqrt(lengthSquared);
				if (!(std::fabs(length - 1.0) <= UnitQuaternionTolerance))
				{
					issues.push_back({ .JsonPointer = pointer, .Message = std::format("must be a unit quaternion (length {})", FormatNumber(length)), .Hint = "normalize the rotation", .Suggestions = {} });
				}
			}
		}

		// The field's value in its kind's canonical JSON form.
		static Json FieldToJson(const NativeFieldInfo& field, std::span<const std::byte> value)
		{
			switch (field.Kind)
			{
				case NativeFieldKind::Bool:
					// Read as a byte: a module's bool member holding another value than 0 or 1 is read as true, never as UB.
					return Json(LoadMember<uint8_t>(value, field.Offset) != 0);
				case NativeFieldKind::Int32:
					return Json(static_cast<int64_t>(LoadMember<int32_t>(value, field.Offset)));
				case NativeFieldKind::UInt32:
					return Json(static_cast<uint64_t>(LoadMember<uint32_t>(value, field.Offset)));
				case NativeFieldKind::Entity:
				{
					const auto entity = LoadMember<NativeEntity>(value, field.Offset);
					return entity.IsValid() ? Json(UUID(entity.Id).ToString()) : Json(nullptr);
				}
				case NativeFieldKind::Enum:
				{
					const int32_t number = LoadMember<int32_t>(value, field.Offset);
					// A value without an entry (only a module's system can write one) stays a number, which reads back as
					// a reset note: NativeWorld refuses the system's write (Session/NativeWorld.cpp, FindInvalidValue).
					if (const NativeEnumValue* entry = FindEnumValue(field, number); entry != nullptr)
						return Json(entry->Name);
					return Json(static_cast<int64_t>(number));
				}
				case NativeFieldKind::Float:
					return Json(static_cast<double>(LoadMember<float>(value, field.Offset)));
				case NativeFieldKind::Vec2:
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Quat:
				case NativeFieldKind::Color3:
				case NativeFieldKind::Color4:
					break;
			}
			const std::array<float, 4> floats = LoadFloats(field, value);
			Json array = Json::array();
			for (uint32_t index = 0; index < GetKindLayout(field.Kind).FloatCount; ++index)
				array.push_back(static_cast<double>(floats[index]));
			return array;
		}

		// How reading one member into its field went.
		enum class FieldReadOutcome : uint8_t
		{
			Read,    // the field holds the member's value (which may still break a rule: see the issues)
			Invalid, // the member is of the field's form but no value of the field (an entity or enum name it rejects)
			Reset    // the member does not convert to the field's kind: the field keeps its default
		};

		// An issue from a JsonReader error located in `pointer`'s frame.
		static ErrorIssue MakeIssue(const Error& error, const std::string& pointer)
		{
			return ErrorIssue{ .JsonPointer = error.GetLocation().JsonPointer.value_or(pointer), .Message = error.GetMessageText(), .Hint = error.GetHint(), .Suggestions = {} };
		}

		// The integer a JSON number stands for exactly: an integer, or a float without a fraction within int64.
		static std::optional<int64_t> ReadExactInteger(const Json& member)
		{
			const JsonReader reader(member);
			switch (GetJsonType(member))
			{
				case JsonType::Integer:
				{
					const Result<int64_t> number = reader.ReadInt64();
					return number.has_value() ? std::optional<int64_t>(*number) : std::nullopt;
				}
				case JsonType::Float:
				{
					const Result<double> number = reader.ReadDouble();
					// 2^63 is the first double above int64's range.
					if (!number.has_value() || std::floor(*number) != *number || *number < -9223372036854775808.0 || *number >= 9223372036854775808.0)
						return std::nullopt;
					return static_cast<int64_t>(*number);
				}
				case JsonType::Null:
				case JsonType::Bool:
				case JsonType::String:
				case JsonType::Array:
				case JsonType::Object:
					break;
			}
			return std::nullopt;
		}

		// Reads `member` into the field (decision 27): a member of the field's form is read, one of another kind's form that
		// converts exactly is converted, and anything else is a Reset; the field's rules are checked afterwards (CheckField).
		static FieldReadOutcome ReadField(const NativeFieldInfo& field, const Json& member, const std::string& pointer, std::span<std::byte> value,
			std::vector<ErrorIssue>& issues)
		{
			const JsonType type = GetJsonType(member);
			switch (field.Kind)
			{
				case NativeFieldKind::Bool:
				{
					const Result<bool> flag = JsonReader(member).ReadBool();
					if (type != JsonType::Bool || !flag.has_value())
						return FieldReadOutcome::Reset;
					StoreMember(value, field.Offset, *flag);
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::Int32:
				{
					const std::optional<int64_t> number = ReadExactInteger(member);
					if (!number.has_value() || *number < std::numeric_limits<int32_t>::min() || *number > std::numeric_limits<int32_t>::max())
						return FieldReadOutcome::Reset;
					StoreMember(value, field.Offset, static_cast<int32_t>(*number));
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::UInt32:
				{
					// A non-negative integer beyond int64 is no uint32 either.
					const std::optional<int64_t> number = ReadExactInteger(member);
					if (!number.has_value() || *number < 0 || *number > std::numeric_limits<uint32_t>::max())
						return FieldReadOutcome::Reset;
					StoreMember(value, field.Offset, static_cast<uint32_t>(*number));
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::Entity:
				{
					if (type == JsonType::Null)
					{
						StoreMember(value, field.Offset, NativeEntity{});
						return FieldReadOutcome::Read;
					}
					if (type != JsonType::String)
						return FieldReadOutcome::Reset;
					const Result<std::string> text = JsonReader(member).ReadString();
					const bool lowercaseHex = text.has_value() && text->size() == 16 && std::ranges::all_of(*text, [](char character)
					{
						return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
					});
					const std::optional<UUID> id = lowercaseHex ? UUID::FromString(*text) : std::nullopt;
					if (!id.has_value())
					{
						issues.push_back({ .JsonPointer = pointer, .Message = "an entity must be 16 lowercase hex digits or null", .Hint = {}, .Suggestions = {} });
						return FieldReadOutcome::Invalid;
					}
					StoreMember(value, field.Offset, NativeEntity{ id->GetValue() });
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::Enum:
				{
					if (type == JsonType::String)
					{
						const Result<std::string> name = JsonReader(member).ReadString();
						const NativeEnumValue* entry = name.has_value() ? FindEnumName(field, *name) : nullptr;
						if (entry == nullptr)
						{
							std::vector<std::string> names;
							for (const NativeEnumValue& candidate : field.EnumValues)
								names.push_back(candidate.Name);
							std::vector<std::string> suggestions = FuzzySuggest(name.value_or(std::string()), names);
							std::string hint = MakeDidYouMeanHint(suggestions);
							issues.push_back({ .JsonPointer = pointer,
								.Message = std::format("'{}' is not an entry of '{}'", name.value_or(std::string()), field.Name),
								.Hint = std::move(hint),
								.Suggestions = std::move(suggestions) });
							return FieldReadOutcome::Invalid;
						}
						StoreMember(value, field.Offset, entry->Value);
						return FieldReadOutcome::Read;
					}
					// A number converts exactly when it is an entry's value (a field that was an Int32 before).
					const std::optional<int64_t> number = ReadExactInteger(member);
					if (!number.has_value() || FindEnumValue(field, *number) == nullptr)
						return FieldReadOutcome::Reset;
					StoreMember(value, field.Offset, static_cast<int32_t>(*number));
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::Float:
				{
					if (type != JsonType::Integer && type != JsonType::Float)
						return FieldReadOutcome::Reset;
					// Every number reads as the float nearest to it, as authored numbers do (§6); one beyond float's range is
					// not finite.
					const Result<float> number = JsonReader(member, pointer).ReadFloat();
					if (!number.has_value())
					{
						issues.push_back(MakeIssue(number.error(), pointer));
						return FieldReadOutcome::Invalid;
					}
					StoreMember(value, field.Offset, *number);
					return FieldReadOutcome::Read;
				}
				case NativeFieldKind::Vec2:
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Quat:
				case NativeFieldKind::Color3:
				case NativeFieldKind::Color4:
					break;
			}

			const uint32_t count = GetKindLayout(field.Kind).FloatCount;
			if (type != JsonType::Array || member.size() != count)
				return FieldReadOutcome::Reset;
			for (const Json& element : member)
			{
				const JsonType elementType = GetJsonType(element);
				if (elementType != JsonType::Integer && elementType != JsonType::Float)
					return FieldReadOutcome::Reset;
			}
			std::array<float, 4> floats{};
			bool readAll = true;
			const JsonReader array(member, pointer);
			for (uint32_t index = 0; index < count; ++index)
			{
				const Result<JsonReader> element = array.GetElement(index);
				const Result<float> number = element.has_value() ? element->ReadFloat() : Result<float>(std::unexpected(element.error()));
				if (!number.has_value())
				{
					issues.push_back(MakeIssue(number.error(), JsonReader::AppendPointer(pointer, index)));
					readAll = false;
					continue;
				}
				floats[index] = *number;
			}
			if (!readAll)
				return FieldReadOutcome::Invalid;
			StoreFloats(field, value, floats);
			return FieldReadOutcome::Read;
		}

		static const NativeFieldInfo* FindFieldByName(const NativeTypeInfo& type, std::string_view name)
		{
			const auto found = std::ranges::find(type.Fields, name, &NativeFieldInfo::Name);
			return found == type.Fields.end() ? nullptr : &*found;
		}

		static const NativeFieldInfo* FindFieldByRenamedFrom(const NativeTypeInfo& type, std::string_view name)
		{
			const auto found = std::ranges::find_if(type.Fields, [name](const NativeFieldInfo& field)
			{
				return !field.RenamedFrom.empty() && field.RenamedFrom == name;
			});
			return found == type.Fields.end() ? nullptr : &*found;
		}

		// NativeValueFromJson's work, without the Changed comparison.
		static Result<NativeValueReadReport> ReadValue(const NativeTypeInfo& type, const Json& json, std::string_view pointer, std::span<std::byte> value)
		{
			ENGINE_CORE_ASSERT(value.size() == type.Size, "a value of the native type '{}' is {} bytes (got {})", type.RegistryName, type.Size, value.size());
			ENGINE_CORE_ASSERT(type.Defaults.size() == type.Size, "the native type '{}' has no defaults", type.RegistryName);
			if (!json.is_object())
			{
				std::string message = std::format("expected an object holding the fields of {}, got {}", type.RegistryName, JsonTypeToString(GetJsonType(json)));
				return std::unexpected(Error(ErrorCode::Validation, message)
						.WithLocation({ .File = {}, .Line = 0, .Column = 0, .JsonPointer = std::string(pointer), .Entity = {} })
						.WithIssue({ .JsonPointer = std::string(pointer), .Message = message, .Hint = {}, .Suggestions = {} }));
			}

			std::ranges::copy(type.Defaults, value.begin());
			NativeValueReadReport report;
			std::vector<ErrorIssue> issues;
			const JsonReader object(json);
			for (auto member = json.begin(); member != json.end(); ++member)
			{
				const std::string& name = member.key();
				const std::string memberPointer = JsonReader::AppendPointer(pointer, name);
				const NativeFieldInfo* field = FindFieldByName(type, name);
				if (field == nullptr)
				{
					field = FindFieldByRenamedFrom(type, name);
					if (field != nullptr && object.HasMember(field->Name))
						field = nullptr;
				}
				if (field == nullptr)
				{
					report.Notes.push_back({ .Code = std::string(NativeFieldDroppedCode),
						.Field = name,
						.Message = std::format("dropped '{}': {} has no such field", name, type.RegistryName) });
					continue;
				}

				const size_t issueCount = issues.size();
				const FieldReadOutcome outcome = ReadField(*field, member.value(), memberPointer, value, issues);
				if (outcome == FieldReadOutcome::Reset)
				{
					report.Notes.push_back({ .Code = std::string(NativeFieldResetCode),
						.Field = name,
						.Message = std::format("reset '{}' of {} to its default: it must be {}, got {}", field->Name, type.RegistryName, DescribeKindForm(field->Kind),
							JsonTypeToString(GetJsonType(member.value()))) });
					continue;
				}
				if (outcome == FieldReadOutcome::Read && issues.size() == issueCount)
					CheckField(*field, value, memberPointer, issues);
			}

			if (!issues.empty())
			{
				return std::unexpected(Error(ErrorCode::Validation, std::format("{} invalid value(s) in {}", issues.size(), type.RegistryName))
						.WithLocation({ .File = {}, .Line = 0, .Column = 0, .JsonPointer = std::string(pointer), .Entity = {} })
						.WithIssues(std::move(issues)));
			}
			return report;
		}

		// A double for a JSON Schema keyword: an integer when it is integral and fits int64 (as JsonSchema writes them).
		static Json MakeSchemaNumber(double value)
		{
			if (std::floor(value) == value && std::fabs(value) < 9.0e18)
				return Json(static_cast<int64_t>(value));
			return Json(value);
		}

		static Json MakeNumberSchema(const NativeFieldInfo& field, bool integer, double typeMinimum, double typeMaximum)
		{
			const auto bound = [integer](double number)
			{
				return MakeSchemaNumber(integer ? number : ToFloatBound(number));
			};
			Json schema = Json::object();
			schema["type"] = integer ? "integer" : "number";
			if (integer || field.Min.has_value())
				schema["minimum"] = bound(field.Min.has_value() ? std::max(*field.Min, typeMinimum) : typeMinimum);
			if (integer || field.Max.has_value())
				schema["maximum"] = bound(field.Max.has_value() ? std::min(*field.Max, typeMaximum) : typeMaximum);
			return schema;
		}

		// The schema of one field: JsonSchema's mapping of the matching built-in kind (Reflection/JsonSchema.h), with the
		// field's kind and unit appended to its description, since the schema keywords the engine validates name neither.
		static Json MakeFieldSchema(const NativeFieldInfo& field)
		{
			Json schema = Json::object();
			schema["description"] = field.Unit.empty() ? std::format("{} ({})", field.Description, GetKindName(field.Kind))
													   : std::format("{} ({}, {})", field.Description, GetKindName(field.Kind), field.Unit);
			const double floatMaximum = static_cast<double>(std::numeric_limits<float>::max());
			Json body;
			switch (field.Kind)
			{
				case NativeFieldKind::Bool:
					body = Json::object({ { "type", "boolean" } });
					break;
				case NativeFieldKind::Int32:
					body = MakeNumberSchema(field, true, static_cast<double>(std::numeric_limits<int32_t>::min()), static_cast<double>(std::numeric_limits<int32_t>::max()));
					break;
				case NativeFieldKind::UInt32:
					body = MakeNumberSchema(field, true, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max()));
					break;
				case NativeFieldKind::Float:
					body = MakeNumberSchema(field, false, -floatMaximum, floatMaximum);
					break;
				case NativeFieldKind::Vec2:
				case NativeFieldKind::Vec3:
				case NativeFieldKind::Vec4:
				case NativeFieldKind::Quat:
				case NativeFieldKind::Color3:
				case NativeFieldKind::Color4:
				{
					const uint32_t count = GetKindLayout(field.Kind).FloatCount;
					Json items = Json::array();
					for (uint32_t index = 0; index < count; ++index)
						items.push_back(MakeNumberSchema(field, false, -floatMaximum, floatMaximum));
					body = Json::object();
					body["type"] = "array";
					body["prefixItems"] = std::move(items);
					body["minItems"] = count;
					body["maxItems"] = count;
					break;
				}
				case NativeFieldKind::Entity:
				{
					body = Json::object();
					body["type"] = Json::array({ "string", "null" });
					body["pattern"] = "^[0-9a-f]{16}$";
					break;
				}
				case NativeFieldKind::Enum:
				{
					Json names = Json::array();
					for (const NativeEnumValue& entry : field.EnumValues)
						names.push_back(entry.Name);
					body = Json::object();
					body["enum"] = std::move(names);
					break;
				}
			}
			for (auto member = body.begin(); member != body.end(); ++member)
				schema[member.key()] = std::move(member.value());
			return schema;
		}

		static void HashText(XXH64Hasher& hasher, std::string_view text)
		{
			hasher.UpdateU64(text.size());
			hasher.Update(text);
		}

		static void HashBound(XXH64Hasher& hasher, const std::optional<double>& bound)
		{
			hasher.UpdateU64(bound.has_value() ? 1 : 0);
			hasher.UpdateU64(bound.has_value() ? std::bit_cast<uint64_t>(*bound) : 0);
		}

		// NativeModuleTypes::GetLayoutHash's input for one type.
		static void HashLayout(XXH64Hasher& hasher, const NativeTypeInfo& type)
		{
			hasher.UpdateU64(static_cast<uint64_t>(type.Role));
			HashText(hasher, type.Name);
			hasher.UpdateU64(type.Size);
			hasher.UpdateU64(type.Alignment);
			hasher.UpdateU64(type.Fields.size());
			for (const NativeFieldInfo& field : type.Fields)
			{
				HashText(hasher, field.Name);
				hasher.UpdateU64(static_cast<uint64_t>(field.Kind));
				hasher.UpdateU64(field.Offset);
				HashBound(hasher, field.Min);
				HashBound(hasher, field.Max);
				HashText(hasher, field.RenamedFrom);
				hasher.UpdateU64(field.EnumValues.size());
				for (const NativeEnumValue& entry : field.EnumValues)
				{
					HashText(hasher, entry.Name);
					hasher.UpdateU64(static_cast<uint64_t>(static_cast<int64_t>(entry.Value)));
				}
			}
			const Result<std::string> defaults = JsonWriter::Write(NativeValueToJson(type, type.Defaults), JsonStyle::Minified);
			HashText(hasher, defaults.has_value() ? std::string_view(*defaults) : std::string_view("<unwritable defaults>"));
		}

		// Receives a module's registration (BuildNativeModuleTypes): validates each type and system as it arrives and copies
		// it, since the descriptors' views live only during the call; the first broken rule is kept and everything after it
		// ignored.
		class NativeRegistrar final : public INativeRegistrar
		{
		public:
			void AddType(const NativeTypeDescriptor& type) override
			{
				if (m_Error.has_value())
					return;
				Result<NativeTypeInfo> info = CopyType(type);
				if (!info.has_value())
				{
					m_Error = std::move(info).error();
					return;
				}
				std::vector<NativeTypeInfo>& list = info->Role == NativeTypeRole::Component ? m_Components : m_Resources;
				info->Index = static_cast<uint32_t>(list.size());
				list.push_back(std::move(*info));
			}

			void AddSystem(const NativeSystemDescriptor& system) override
			{
				if (m_Error.has_value())
					return;
				const Status checked = CheckSystem(system);
				if (!checked.has_value())
				{
					m_Error = checked.error();
					return;
				}
				m_Systems.push_back({ .Name = std::string(system.Name), .Description = std::string(system.Description), .Run = system.Run });
			}

			[[nodiscard]] const std::optional<Error>& GetError() const { return m_Error; }
			[[nodiscard]] bool IsEmpty() const { return m_Components.empty() && m_Resources.empty() && m_Systems.empty(); }

			std::vector<NativeTypeInfo> TakeComponents() { return std::move(m_Components); }
			std::vector<NativeTypeInfo> TakeResources() { return std::move(m_Resources); }
			std::vector<NativeSystemInfo> TakeSystems() { return std::move(m_Systems); }
		private:
			[[nodiscard]] static Error MakeRuleError(std::string message, std::string hint = {})
			{
				Error error(ErrorCode::Validation, std::move(message));
				if (!hint.empty())
					return std::move(error).WithHint(std::move(hint));
				return error;
			}

			[[nodiscard]] static Status CheckName(std::string_view name, std::string_view what)
			{
				if (IsValidNativeName(name))
					return {};
				return std::unexpected(MakeRuleError(std::format("the native {} name '{}' is not PascalCase ASCII ([A-Z][A-Za-z0-9]*) of at most {} bytes", what,
														 SanitizeUtf8(name, MaxNativeNameBytes * 2), MaxNativeNameBytes),
					"rename it, for example 'Spinner'"));
			}

			[[nodiscard]] static Status CheckDescription(std::string_view description, std::string_view subject)
			{
				if (description.empty())
					return std::unexpected(MakeRuleError(std::format("{} has an empty description", subject), "describe it in one sentence"));
				if (!IsValidUtf8(description))
					return std::unexpected(MakeRuleError(std::format("the description of {} is not valid UTF-8", subject)));
				return {};
			}

			[[nodiscard]] Status CheckSystem(const NativeSystemDescriptor& system) const
			{
				ENGINE_TRY(CheckName(system.Name, "system"));
				const std::string subject = std::format("the native system '{}'", system.Name);
				if (m_Systems.size() >= MaxNativeSystems)
					return std::unexpected(MakeRuleError(std::format("{} is one system more than the {} a module may register", subject, MaxNativeSystems)));
				if (std::ranges::find(m_Systems, system.Name, &NativeSystemInfo::Name) != m_Systems.end())
					return std::unexpected(MakeRuleError(std::format("the module registers two systems named '{}'", system.Name)));
				ENGINE_TRY(CheckDescription(system.Description, subject));
				if (system.Run == nullptr)
					return std::unexpected(MakeRuleError(std::format("{} has no Run function", subject)));
				return {};
			}

			[[nodiscard]] Result<NativeTypeInfo> CopyType(const NativeTypeDescriptor& type) const
			{
				ENGINE_TRY(CheckName(type.Name, "type"));
				const std::string subject = std::format("the native type '{}'", type.Name);
				if (m_Components.size() + m_Resources.size() >= MaxNativeTypes)
					return std::unexpected(MakeRuleError(std::format("{} is one type more than the {} a module may register", subject, MaxNativeTypes)));
				if (type.Role != NativeTypeRole::Component && type.Role != NativeTypeRole::Resource)
					return std::unexpected(MakeRuleError(std::format("{} has an unknown role", subject)));
				const std::vector<NativeTypeInfo>& list = type.Role == NativeTypeRole::Component ? m_Components : m_Resources;
				if (std::ranges::find(list, type.Name, &NativeTypeInfo::Name) != list.end())
				{
					return std::unexpected(MakeRuleError(std::format("the module registers two {} types named '{}'",
						type.Role == NativeTypeRole::Component ? "component" : "resource", type.Name)));
				}
				ENGINE_TRY(CheckDescription(type.Description, subject));
				if (type.Size == 0 || type.Size > MaxNativeTypeSize)
					return std::unexpected(MakeRuleError(std::format("{} is {} bytes; a native type is 1 to {} bytes", subject, type.Size, MaxNativeTypeSize)));
				if (!std::has_single_bit(type.Alignment) || type.Alignment > MaxNativeTypeAlignment || type.Size % type.Alignment != 0)
				{
					return std::unexpected(MakeRuleError(std::format("{} has the alignment {}; it must be a power of two up to {} that divides its size {}", subject,
						type.Alignment, MaxNativeTypeAlignment, type.Size)));
				}
				if (type.Defaults == nullptr)
					return std::unexpected(MakeRuleError(std::format("{} has no defaults", subject)));
				if (type.Fields.size() > MaxNativeFieldsPerType)
				{
					return std::unexpected(MakeRuleError(std::format("{} has {} fields; a native type has at most {}", subject, type.Fields.size(), MaxNativeFieldsPerType)));
				}

				NativeTypeInfo info;
				info.Role = type.Role;
				info.Name = std::string(type.Name);
				info.RegistryName = type.Role == NativeTypeRole::Component ? std::format("{}{}", NativeComponentPrefix, type.Name) : info.Name;
				info.Description = std::string(type.Description);
				info.Size = type.Size;
				info.Alignment = type.Alignment;
				info.Defaults.resize(type.Size);
				std::memcpy(info.Defaults.data(), type.Defaults, type.Size);
				for (const NativeFieldDescriptor& field : type.Fields)
				{
					ENGINE_TRY_ASSIGN(NativeFieldInfo copy, CopyField(info, field));
					info.Fields.push_back(std::move(copy));
				}
				ENGINE_TRY(CheckRenames(info));
				ENGINE_TRY(CheckDefaults(info));
				return info;
			}

			[[nodiscard]] static Result<NativeFieldInfo> CopyField(const NativeTypeInfo& type, const NativeFieldDescriptor& field)
			{
				ENGINE_TRY(CheckName(field.Name, std::format("field (of the native type '{}')", type.Name)));
				const std::string subject = std::format("the field '{}' of the native type '{}'", field.Name, type.Name);
				if (std::ranges::find(type.Fields, field.Name, &NativeFieldInfo::Name) != type.Fields.end())
					return std::unexpected(MakeRuleError(std::format("the native type '{}' has two fields named '{}'", type.Name, field.Name)));
				ENGINE_TRY(CheckDescription(field.Description, subject));
				if (!IsKnownKind(field.Kind))
					return std::unexpected(MakeRuleError(std::format("{} has an unknown kind {}", subject, static_cast<uint32_t>(field.Kind))));

				const NativeKindLayout layout = GetKindLayout(field.Kind);
				if (field.Offset > type.Size || layout.Size > type.Size - field.Offset)
				{
					return std::unexpected(MakeRuleError(std::format("{} ({} bytes at offset {}) lies outside the type's {} bytes", subject, layout.Size, field.Offset, type.Size)));
				}
				if (field.Offset % layout.Alignment != 0)
					return std::unexpected(MakeRuleError(std::format("{} at offset {} is not aligned to its kind's {} bytes", subject, field.Offset, layout.Alignment)));
				for (const NativeFieldInfo& earlier : type.Fields)
				{
					const uint32_t earlierSize = GetKindLayout(earlier.Kind).Size;
					if (field.Offset < earlier.Offset + earlierSize && earlier.Offset < field.Offset + layout.Size)
						return std::unexpected(MakeRuleError(std::format("{} overlaps the field '{}'", subject, earlier.Name)));
				}

				const NativeFieldOptions& options = field.Options;
				const bool bounded = options.Min.has_value() || options.Max.has_value();
				if (bounded && (field.Kind == NativeFieldKind::Bool || field.Kind == NativeFieldKind::Entity || field.Kind == NativeFieldKind::Enum))
					return std::unexpected(MakeRuleError(std::format("{} is a {} and has no numbers to bound", subject, GetKindName(field.Kind))));
				if ((options.Min.has_value() && !std::isfinite(*options.Min)) || (options.Max.has_value() && !std::isfinite(*options.Max)))
					return std::unexpected(MakeRuleError(std::format("{} has a bound that is not finite", subject)));
				if (options.Min.has_value() && options.Max.has_value() && *options.Min > *options.Max)
				{
					return std::unexpected(MakeRuleError(std::format("{} has Min {} above Max {}", subject, FormatNumber(*options.Min), FormatNumber(*options.Max))));
				}
				if (!IsValidUtf8(options.Unit))
					return std::unexpected(MakeRuleError(std::format("the unit of {} is not valid UTF-8", subject)));
				if (!options.RenamedFrom.empty() && !IsValidNativeName(options.RenamedFrom))
					return std::unexpected(MakeRuleError(std::format("{} was renamed from '{}', which is not a valid field name", subject, SanitizeUtf8(options.RenamedFrom, 128))));

				NativeFieldInfo info;
				info.Name = std::string(field.Name);
				info.Description = std::string(field.Description);
				info.Kind = field.Kind;
				info.Offset = field.Offset;
				info.Min = options.Min;
				info.Max = options.Max;
				info.Unit = std::string(options.Unit);
				info.RenamedFrom = std::string(options.RenamedFrom);

				if (field.Kind == NativeFieldKind::Enum)
				{
					if (field.EnumEntries.empty() || field.EnumEntries.size() > MaxNativeEnumEntries)
						return std::unexpected(MakeRuleError(std::format("{} is an enum with {} entries; it needs 1 to {}", subject, field.EnumEntries.size(), MaxNativeEnumEntries)));
					for (const NativeEnumEntry& entry : field.EnumEntries)
					{
						ENGINE_TRY(CheckName(entry.Name, std::format("enum entry (of {})", subject)));
						const std::string entrySubject = std::format("the entry '{}' of {}", entry.Name, subject);
						if (std::ranges::find(info.EnumValues, entry.Name, &NativeEnumValue::Name) != info.EnumValues.end())
							return std::unexpected(MakeRuleError(std::format("{} has two entries named '{}'", subject, entry.Name)));
						if (std::ranges::find(info.EnumValues, entry.Value, &NativeEnumValue::Value) != info.EnumValues.end())
							return std::unexpected(MakeRuleError(std::format("{} has the value {} of an earlier entry", entrySubject, entry.Value)));
						ENGINE_TRY(CheckDescription(entry.Description, entrySubject));
						info.EnumValues.push_back({ .Name = std::string(entry.Name), .Value = entry.Value, .Description = std::string(entry.Description) });
					}
				}
				else if (!field.EnumEntries.empty())
					return std::unexpected(MakeRuleError(std::format("{} is a {} and has enum entries", subject, GetKindName(field.Kind))));
				return info;
			}

			// RenamedFrom names no field of the type and no other field's RenamedFrom.
			[[nodiscard]] static Status CheckRenames(const NativeTypeInfo& type)
			{
				for (size_t index = 0; index < type.Fields.size(); ++index)
				{
					const NativeFieldInfo& field = type.Fields[index];
					if (field.RenamedFrom.empty())
						continue;
					if (FindFieldByName(type, field.RenamedFrom) != nullptr)
					{
						return std::unexpected(MakeRuleError(std::format("the field '{}' of the native type '{}' was renamed from '{}', which names a field of the type",
							field.Name, type.Name, field.RenamedFrom)));
					}
					for (size_t earlier = 0; earlier < index; ++earlier)
					{
						if (type.Fields[earlier].RenamedFrom == field.RenamedFrom)
						{
							return std::unexpected(MakeRuleError(std::format("the fields '{}' and '{}' of the native type '{}' were both renamed from '{}'",
								type.Fields[earlier].Name, field.Name, type.Name, field.RenamedFrom)));
						}
					}
				}
				return {};
			}

			// Each field's default is a value the field accepts, and an Entity's is none.
			[[nodiscard]] static Status CheckDefaults(const NativeTypeInfo& type)
			{
				for (const NativeFieldInfo& field : type.Fields)
				{
					std::vector<ErrorIssue> issues;
					CheckField(field, type.Defaults, std::format("/{}", field.Name), issues);
					if (field.Kind == NativeFieldKind::Entity && LoadMember<NativeEntity>(type.Defaults, field.Offset).IsValid())
						issues.push_back({ .JsonPointer = {}, .Message = "an entity field's default is none", .Hint = {}, .Suggestions = {} });
					if (!issues.empty())
					{
						return std::unexpected(MakeRuleError(std::format("the default of the field '{}' of the native type '{}' is invalid: {}", field.Name, type.Name,
							issues.front().Message)));
					}
				}
				return {};
			}
		private:
			std::vector<NativeTypeInfo> m_Components{};
			std::vector<NativeTypeInfo> m_Resources{};
			std::vector<NativeSystemInfo> m_Systems{};
			std::optional<Error> m_Error{};
		};

		// The largest exception message a refusal keeps.
		inline constexpr size_t MaxNativeExceptionMessageBytes = 1024;

	}

	NativeModuleTypes::NativeModuleTypes(ConstructionKey /*key*/, std::vector<NativeTypeInfo> components, std::vector<NativeTypeInfo> resources,
		std::vector<NativeSystemInfo> systems, Ref<const void> keepalive)
		: m_Components(std::move(components)), m_Resources(std::move(resources)), m_Systems(std::move(systems)), m_Keepalive(std::move(keepalive))
	{
		XXH64Hasher hasher(0);
		hasher.UpdateU64(m_Components.size());
		for (const NativeTypeInfo& type : m_Components)
			Utils::HashLayout(hasher, type);
		hasher.UpdateU64(m_Resources.size());
		for (const NativeTypeInfo& type : m_Resources)
			Utils::HashLayout(hasher, type);
		m_LayoutHash = hasher.Digest();
	}

	const NativeTypeInfo* NativeModuleTypes::FindComponent(std::string_view registryName) const
	{
		const auto found = std::ranges::find(m_Components, registryName, &NativeTypeInfo::RegistryName);
		return found == m_Components.end() ? nullptr : &*found;
	}

	const NativeTypeInfo* NativeModuleTypes::FindResource(std::string_view name) const
	{
		const auto found = std::ranges::find(m_Resources, name, &NativeTypeInfo::Name);
		return found == m_Resources.end() ? nullptr : &*found;
	}

	Result<Ref<const NativeModuleTypes>> BuildNativeModuleTypes(const NativeModuleEntryPoint& entry, Ref<const void> keepalive)
	{
		if (entry.ApiVersion != NativeApiVersion)
		{
			return std::unexpected(Error(ErrorCode::UnsupportedVersion,
				std::format("the native module was built against interface version {}, but this engine implements version {}", entry.ApiVersion, NativeApiVersion))
					.WithHint("rebuild the module against this engine's NativeApi.h (native.build)"));
		}
		if (entry.Register == nullptr)
			return MakeError(ErrorCode::InvalidArgument, "the native module has no registration function (ENGINE_NATIVE_MODULE names none)");

		// §4.6 boundary (item 8): the registration is the module's code. An exception that escapes it refuses the module,
		// and the registrar, which only it filled, is discarded with everything it registered.
		Utils::NativeRegistrar registrar;
		try
		{
			entry.Register(registrar);
		}
		catch (const std::exception& exception)
		{
			return std::unexpected(Error(ErrorCode::Validation,
				std::format("the registration of the native module raised an exception: {}", Utils::SanitizeUtf8(exception.what(), Utils::MaxNativeExceptionMessageBytes)))
					.WithHint("fix the module's registration function; the module was not loaded"));
		}
		catch (...)
		{
			return std::unexpected(Error(ErrorCode::Validation, "the registration of the native module raised an exception: an exception of an unknown type")
					.WithHint("fix the module's registration function; the module was not loaded"));
		}

		if (const std::optional<Error>& error = registrar.GetError(); error.has_value())
			return std::unexpected(Error(*error).WithContext("while registering the native module"));
		if (registrar.IsEmpty())
		{
			return std::unexpected(Error(ErrorCode::Validation, "the native module registers neither a type nor a system")
					.WithHint("register a component, a resource or a system in the function ENGINE_NATIVE_MODULE names"));
		}
		return CreateRef<const NativeModuleTypes>(NativeModuleTypes::ConstructionKey(), registrar.TakeComponents(), registrar.TakeResources(), registrar.TakeSystems(),
			std::move(keepalive));
	}

	Json NativeValueToJson(const NativeTypeInfo& type, std::span<const std::byte> value)
	{
		ENGINE_CORE_ASSERT(value.size() == type.Size, "a value of the native type '{}' is {} bytes (got {})", type.RegistryName, type.Size, value.size());
		Json json = Json::object();
		for (const NativeFieldInfo& field : type.Fields)
			json[field.Name] = Utils::FieldToJson(field, value);
		return json;
	}

	Result<NativeValueReadReport> NativeValueFromJson(const NativeTypeInfo& type, const Json& json, std::string_view pointer, std::span<std::byte> value)
	{
		ENGINE_TRY_ASSIGN(NativeValueReadReport report, Utils::ReadValue(type, json, pointer, value));
		report.Changed = !Utils::HaveSameText(NativeValueToJson(type, value), json);
		return report;
	}

	Result<NativeCanonicalValue> CanonicalizeNativeValue(const NativeTypeInfo& type, const Json& json, std::string_view pointer)
	{
		std::vector<std::byte> value(type.Size);
		ENGINE_TRY_ASSIGN(NativeValueReadReport report, Utils::ReadValue(type, json, pointer, value));
		Json canonical = NativeValueToJson(type, value);
		report.Changed = !Utils::HaveSameText(canonical, json);
		return NativeCanonicalValue{ .Value = VariantValue(std::move(canonical)), .Report = std::move(report) };
	}

	Json GetNativeTypeSchema(const NativeTypeInfo& type)
	{
		Json schema = Json::object();
		schema["$schema"] = std::string(JsonSchema::Dialect);
		schema["title"] = type.RegistryName;
		schema["description"] = type.Description;
		schema["type"] = "object";
		Json properties = Json::object();
		for (const NativeFieldInfo& field : type.Fields)
			properties[field.Name] = Utils::MakeFieldSchema(field);
		schema["properties"] = std::move(properties);
		schema["additionalProperties"] = false;
		return schema;
	}

	void AppendNativeValueHash(const NativeTypeInfo& type, std::span<const std::byte> value, XXH64Hasher& hasher)
	{
		hasher.Update(type.RegistryName);
		// Validated values always write; the error's text keeps the hash deterministic for one that does not.
		const Result<std::string> canonical = JsonWriter::Write(NativeValueToJson(type, value), JsonStyle::Minified);
		hasher.Update(canonical.has_value() ? *canonical : canonical.error().ToString());
	}

}
