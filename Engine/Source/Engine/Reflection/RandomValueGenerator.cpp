#include "EnginePCH.h"
#include "Engine/Reflection/RandomValueGenerator.h"

#include "Engine/Core/Assert.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Reflection/Private/ReflectionWalk.h"
#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		// The range of unbounded numbers (RandomValueGenerator.h).
		static constexpr double DefaultRandomMagnitude = 1000.0;
		// Free-form Variant values nest arrays and objects at most this deep.
		static constexpr int64_t MaxFreeFormDepth = 3;
		static constexpr int64_t MaxFreeFormElements = 3;
		static constexpr double NullReferenceProbability = 0.25;
		static constexpr std::string_view IdentifierCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";

		struct NumberRange
		{
			double Min = 0.0;
			double Max = 0.0;
		};

		// The intervals a number may be drawn from: [Min, Max] (the metadata, or the default range), with
		// (-MinMagnitude, MinMagnitude) cut out, clamped to [typeMin, typeMax]. One or two intervals; none for contradictory
		// metadata (asserted).
		static std::vector<NumberRange> GetAllowedRanges(const FieldMeta* meta, double typeMin, double typeMax, bool integer)
		{
			const std::optional<double> metaMin = meta != nullptr ? meta->Min : std::nullopt;
			const std::optional<double> metaMax = meta != nullptr ? meta->Max : std::nullopt;
			double low = metaMin.value_or(-DefaultRandomMagnitude);
			double high = metaMax.value_or(DefaultRandomMagnitude);
			if (metaMin.has_value() && !metaMax.has_value() && high < low)
				high = low + DefaultRandomMagnitude;
			if (metaMax.has_value() && !metaMin.has_value() && low > high)
				low = high - DefaultRandomMagnitude;
			low = std::max(low, typeMin);
			high = std::min(high, typeMax);
			if (integer)
			{
				low = std::ceil(low);
				high = std::floor(high);
			}

			const double magnitude = meta != nullptr ? meta->MinMagnitude.value_or(0.0) : 0.0;
			std::vector<NumberRange> ranges;
			if (magnitude <= 0.0)
			{
				if (low <= high)
					ranges.push_back({ low, high });
			}
			else
			{
				const double negativeHigh = integer ? std::min(high, -std::ceil(magnitude)) : std::min(high, -magnitude);
				const double positiveLow = integer ? std::max(low, std::ceil(magnitude)) : std::max(low, magnitude);
				if (low <= negativeHigh)
					ranges.push_back({ low, negativeHigh });
				if (positiveLow <= high)
					ranges.push_back({ positiveLow, high });
			}
			ENGINE_CORE_ASSERT(!ranges.empty(), "Field metadata admits no value (Min/Max/MinMagnitude contradict each other)");
			return ranges;
		}

		static NumberRange PickRange(Random& random, std::span<const NumberRange> ranges)
		{
			if (ranges.empty())
				return {};
			const int64_t index = ranges.size() == 1 ? 0 : random.RangeInt(0, static_cast<int64_t>(ranges.size()) - 1);
			return ranges[static_cast<size_t>(index)];
		}

		static int64_t DrawInteger(Random& random, const FieldMeta* meta, double typeMin, double typeMax)
		{
			const std::vector<NumberRange> ranges = GetAllowedRanges(meta, typeMin, typeMax, true);
			const NumberRange range = PickRange(random, ranges);
			return random.RangeInt(static_cast<int64_t>(range.Min), static_cast<int64_t>(range.Max));
		}

		// A float inside [range.Min, range.Max] after rounding to float (validation compares the float value).
		static float DrawFloatIn(Random& random, NumberRange range)
		{
			float value = static_cast<float>(random.RangeDouble(range.Min, range.Max));
			for (int step = 0; step < 4 && static_cast<double>(value) < range.Min; ++step)
				value = std::nextafter(value, std::numeric_limits<float>::infinity());
			for (int step = 0; step < 4 && static_cast<double>(value) > range.Max; ++step)
				value = std::nextafter(value, -std::numeric_limits<float>::infinity());
			return value;
		}

		static float DrawFloat(Random& random, const FieldMeta* meta)
		{
			const double floatMax = static_cast<double>(std::numeric_limits<float>::max());
			const std::vector<NumberRange> ranges = GetAllowedRanges(meta, -floatMax, floatMax, false);
			return DrawFloatIn(random, PickRange(random, ranges));
		}

		static UUID DrawUUID(Random& random)
		{
			uint64_t value = 0;
			while (value == 0)
				value = random.NextU64();
			return UUID(value);
		}

		static std::string DrawText(Random& random, uint32_t maxLength)
		{
			const auto length = static_cast<size_t>(random.RangeInt(0, maxLength));
			std::string text(length, ' ');
			for (char& character : text)
				character = static_cast<char>(random.RangeInt(0x20, 0x7e));
			return text;
		}

		static std::string DrawIdentifier(Random& random)
		{
			const auto length = static_cast<size_t>(random.RangeInt(1, 8));
			std::string identifier(length, 'A');
			for (size_t i = 0; i < length; ++i)
			{
				// The first character is a letter.
				const int64_t last = static_cast<int64_t>(i == 0 ? 51 : IdentifierCharacters.size() - 1);
				identifier[i] = IdentifierCharacters[static_cast<size_t>(random.RangeInt(0, last))];
			}
			return identifier;
		}

		// A uniformly distributed unit quaternion (rejection sampling in the 4-ball, then normalization).
		static glm::quat DrawUnitQuaternion(Random& random)
		{
			for (int attempt = 0; attempt < 64; ++attempt)
			{
				const double x = random.RangeDouble(-1.0, 1.0);
				const double y = random.RangeDouble(-1.0, 1.0);
				const double z = random.RangeDouble(-1.0, 1.0);
				const double w = random.RangeDouble(-1.0, 1.0);
				const double lengthSquared = x * x + y * y + z * z + w * w;
				if (lengthSquared < 1e-4 || lengthSquared > 1.0)
					continue;
				const double length = std::sqrt(lengthSquared);
				return glm::quat(static_cast<float>(w / length), static_cast<float>(x / length), static_cast<float>(y / length),
					static_cast<float>(z / length));
			}
			return glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
		}

		// A random scalar Value of `type` satisfying `meta` (null for container elements, which have no metadata).
		static Value DrawScalar(Random& random, const TypeInfo& type, const FieldMeta* meta, const RandomValueOptions& options)
		{
			switch (type.GetKind())
			{
				case FieldType::Bool:
					return Value::FromBool(random.NextBool());
				case FieldType::Int32:
					return Value::FromInt32(static_cast<int32_t>(DrawInteger(random, meta, static_cast<double>(std::numeric_limits<int32_t>::min()),
						static_cast<double>(std::numeric_limits<int32_t>::max()))));
				case FieldType::UInt32:
					return Value::FromUInt32(static_cast<uint32_t>(DrawInteger(random, meta, 0.0, static_cast<double>(std::numeric_limits<uint32_t>::max()))));
				case FieldType::Float:
					return Value::FromFloat(DrawFloat(random, meta));
				case FieldType::Vec2:
				{
					const float x = DrawFloat(random, meta);
					const float y = DrawFloat(random, meta);
					return Value::FromVec2(glm::vec2(x, y));
				}
				case FieldType::Vec3:
				case FieldType::Color3:
				{
					const float x = DrawFloat(random, meta);
					const float y = DrawFloat(random, meta);
					const float z = DrawFloat(random, meta);
					const glm::vec3 vector(x, y, z);
					return type.GetKind() == FieldType::Vec3 ? Value::FromVec3(vector) : Value::FromColor3(vector);
				}
				case FieldType::Vec4:
				case FieldType::Color4:
				{
					const float x = DrawFloat(random, meta);
					const float y = DrawFloat(random, meta);
					const float z = DrawFloat(random, meta);
					const float w = DrawFloat(random, meta);
					const glm::vec4 vector(x, y, z, w);
					return type.GetKind() == FieldType::Vec4 ? Value::FromVec4(vector) : Value::FromColor4(vector);
				}
				case FieldType::Quat:
					return Value::FromQuat(DrawUnitQuaternion(random));
				case FieldType::Bool3:
				{
					const bool x = random.NextBool();
					const bool y = random.NextBool();
					const bool z = random.NextBool();
					return Value::FromBool3(glm::bvec3(x, y, z));
				}
				case FieldType::String:
					return Value::FromString(DrawText(random, options.MaxStringLength));
				case FieldType::EntityRef:
				case FieldType::AssetRef:
				{
					const UUID uuid = random.NextBool(NullReferenceProbability) ? UUID() : DrawUUID(random);
					return type.GetKind() == FieldType::EntityRef ? Value::FromEntityRef(uuid) : Value::FromAssetRef(uuid);
				}
				case FieldType::Enum:
				{
					const EnumInfo* enumInfo = type.GetEnum();
					if (enumInfo == nullptr || enumInfo->GetEntries().empty())
						return Value::FromEnum(0);
					const std::span<const EnumEntry> entries = enumInfo->GetEntries();
					const int64_t index = random.RangeInt(0, static_cast<int64_t>(entries.size()) - 1);
					return Value::FromEnum(entries[static_cast<size_t>(index)].Value);
				}
				case FieldType::Variant:
				case FieldType::Array:
				case FieldType::Struct:
				case FieldType::Map:
					break;
			}
			ENGINE_CORE_ASSERT(false, "DrawScalar called for the kind {}", FieldTypeToString(type.GetKind()));
			return {};
		}

		static Json DrawFreeFormJson(Random& random, int64_t depth, uint32_t maxStringLength)
		{
			const int64_t choice = random.RangeInt(0, depth >= MaxFreeFormDepth ? 4 : 6);
			switch (choice)
			{
				case 0:
					return Json(nullptr);
				case 1:
					return Json(random.NextBool());
				case 2:
					return Json(random.RangeInt(-1000, 1000));
				case 3:
					return Json(static_cast<double>(static_cast<float>(random.RangeDouble(-DefaultRandomMagnitude, DefaultRandomMagnitude))));
				case 4:
					return Json(DrawText(random, maxStringLength));
				case 5:
				{
					Json array = Json::array();
					const int64_t count = random.RangeInt(0, MaxFreeFormElements);
					for (int64_t i = 0; i < count; ++i)
						array.push_back(DrawFreeFormJson(random, depth + 1, maxStringLength));
					return array;
				}
				default:
				{
					Json object = Json::object();
					const int64_t count = random.RangeInt(0, MaxFreeFormElements);
					for (int64_t i = 0; i < count; ++i)
						object[DrawIdentifier(random)] = DrawFreeFormJson(random, depth + 1, maxStringLength);
					return object;
				}
			}
		}

	}

	RandomValueGenerator::RandomValueGenerator(const TypeRegistry& registry, uint64_t seed, RandomValueOptions options)
		: m_Registry(&registry), m_Options(std::move(options)), m_Random(seed)
	{
	}

	void RandomValueGenerator::Randomize(const StructInfo& type, void* object)
	{
		ENGINE_CORE_ASSERT(object != nullptr, "Randomize of struct '{}' needs an object", type.GetName());
		ResolveContext owner;
		owner.Registry = m_Registry;
		owner.Owner = object;
		owner.OwnerType = &type;
		owner.Schemas = m_Options.Schemas;
		for (const Scope<FieldInfo>& field : type.GetFields())
		{
			if (!field->IsStored() || field->IsReadOnly() || !field->GetMeta().Serialized)
				continue;
			RandomizeObject(field->GetType(), *field, &field->GetMeta(), field->GetAddress(object), owner);
		}
		type.Generate(object, m_Random);
	}

	Json RandomValueGenerator::RandomJson(const FieldInfo& field, const ResolveContext& context)
	{
		return RandomJsonOf(field.GetType(), field, &field.GetMeta(), context);
	}

	void RandomValueGenerator::RandomizeObject(const TypeInfo& type, const FieldInfo& field, const FieldMeta* meta, void* object,
		const ResolveContext& owner)
	{
		const TypeOps& ops = type.GetOps();
		switch (type.GetKind())
		{
			case FieldType::Variant:
				ops.Write(object, Value::FromVariant(VariantValue(RandomVariantJson(field, owner))));
				return;
			case FieldType::Array:
			{
				const auto count = static_cast<size_t>(m_Random.RangeInt(0, m_Options.MaxArrayLength));
				ops.Resize(object, count);
				ResolveContext elementOwner = owner;
				elementOwner.Key = {};
				const FieldInfo* elementSchema = type.GetElementSchema();
				for (size_t i = 0; i < count; ++i)
					RandomizeObject(*type.GetElement(), elementSchema != nullptr ? *elementSchema : field,
						elementSchema != nullptr ? &elementSchema->GetMeta() : nullptr, ops.GetElement(object, i), elementOwner);
				return;
			}
			case FieldType::Map:
			{
				ops.Clear(object);
				for (const std::string& key : DrawMapKeys(*type.GetElement()))
				{
					ResolveContext valueOwner = owner;
					valueOwner.Key = key;
					RandomizeObject(*type.GetElement(), field, nullptr, ops.FindOrInsert(object, key), valueOwner);
				}
				return;
			}
			case FieldType::Struct:
				Randomize(*type.GetStruct(), object);
				return;
			case FieldType::Bool:
			case FieldType::Int32:
			case FieldType::UInt32:
			case FieldType::Float:
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Quat:
			case FieldType::Color3:
			case FieldType::Color4:
			case FieldType::Bool3:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::AssetRef:
			case FieldType::Enum:
				ops.Write(object, Utils::DrawScalar(m_Random, type, meta, m_Options));
				return;
		}
	}

	Json RandomValueGenerator::RandomJsonOf(const TypeInfo& type, const FieldInfo& field, const FieldMeta* meta, const ResolveContext& context)
	{
		switch (type.GetKind())
		{
			case FieldType::Variant:
				return RandomVariantJson(field, context);
			case FieldType::Array:
			{
				Json array = Json::array();
				const int64_t count = m_Random.RangeInt(0, m_Options.MaxArrayLength);
				ResolveContext elementContext = context;
				elementContext.Key = {};
				const FieldInfo* elementSchema = type.GetElementSchema();
				for (int64_t i = 0; i < count; ++i)
					array.push_back(RandomJsonOf(*type.GetElement(), elementSchema != nullptr ? *elementSchema : field,
						elementSchema != nullptr ? &elementSchema->GetMeta() : nullptr, elementContext));
				return array;
			}
			case FieldType::Map:
			{
				Json map = Json::object();
				std::vector<std::string> keys = DrawMapKeys(*type.GetElement());
				std::sort(keys.begin(), keys.end());
				for (const std::string& key : keys)
				{
					ResolveContext valueContext = context;
					valueContext.Key = key;
					map[key] = RandomJsonOf(*type.GetElement(), field, nullptr, valueContext);
				}
				return map;
			}
			case FieldType::Struct:
			{
				const StructInfo& structType = *type.GetStruct();
				if (type.HasOps())
				{
					const ObjectPtr object = structType.CreateDefault();
					Randomize(structType, object.get());
					Result<Json> json = structType.ToJson(object.get());
					ENGINE_CORE_ASSERT(json.has_value(), "A random '{}' cannot be serialized", structType.GetName());
					return json.has_value() ? std::move(*json) : Json::object();
				}
				// A schema-only struct: one random value per serialized field, without type-level hooks.
				Json object = Json::object();
				ResolveContext memberContext;
				memberContext.Registry = &structType.GetRegistry();
				memberContext.OwnerType = &structType;
				memberContext.Schemas = context.Schemas;
				for (const Scope<FieldInfo>& member : structType.GetFields())
				{
					if (member->IsVirtual() || !member->GetMeta().Serialized)
						continue;
					object[member->GetName()] = RandomJsonOf(member->GetType(), *member, &member->GetMeta(), memberContext);
				}
				return object;
			}
			case FieldType::Bool:
			case FieldType::Int32:
			case FieldType::UInt32:
			case FieldType::Float:
			case FieldType::Vec2:
			case FieldType::Vec3:
			case FieldType::Vec4:
			case FieldType::Quat:
			case FieldType::Color3:
			case FieldType::Color4:
			case FieldType::Bool3:
			case FieldType::String:
			case FieldType::EntityRef:
			case FieldType::AssetRef:
			case FieldType::Enum:
				break;
		}

		Result<Json> json = Utils::ScalarToJson(Utils::DrawScalar(m_Random, type, meta, m_Options), type);
		ENGINE_CORE_ASSERT(json.has_value(), "A random {} cannot be written", FieldTypeToString(type.GetKind()));
		return json.has_value() ? std::move(*json) : Json();
	}

	Json RandomValueGenerator::RandomVariantJson(const FieldInfo& field, const ResolveContext& context)
	{
		if (field.GetResolver() != nullptr)
		{
			const Result<const FieldInfo*> schema = field.GetResolver()(context);
			if (schema.has_value() && *schema != nullptr)
				return RandomJson(**schema, Utils::MakeResolvedSchemaContext(**schema, context));
		}
		return Utils::DrawFreeFormJson(m_Random, 0, m_Options.MaxStringLength);
	}

	std::vector<std::string> RandomValueGenerator::DrawMapKeys(const TypeInfo& element)
	{
		const auto count = static_cast<size_t>(m_Random.RangeInt(0, m_Options.MaxMapKeys));
		std::vector<std::string> keys;
		if (Utils::ContainsVariant(element) && !m_Options.VariantMapKeys.empty())
		{
			// Distinct keys from the options: a partial Fisher-Yates shuffle of their indices.
			std::vector<size_t> order(m_Options.VariantMapKeys.size());
			for (size_t i = 0; i < order.size(); ++i)
				order[i] = i;
			const size_t take = std::min(count, order.size());
			for (size_t i = 0; i < take; ++i)
			{
				const auto pick = static_cast<size_t>(m_Random.RangeInt(static_cast<int64_t>(i), static_cast<int64_t>(order.size()) - 1));
				std::swap(order[i], order[pick]);
				keys.push_back(m_Options.VariantMapKeys[order[i]]);
			}
			return keys;
		}

		for (size_t attempt = 0; keys.size() < count && attempt < count * 4; ++attempt)
		{
			std::string key = Utils::DrawIdentifier(m_Random);
			if (std::find(keys.begin(), keys.end(), key) == keys.end())
				keys.push_back(std::move(key));
		}
		return keys;
	}

}
