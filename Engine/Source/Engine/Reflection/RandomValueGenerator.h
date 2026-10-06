#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/FieldInfo.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class IFieldSchemaSource;
	class StructInfo;
	class TypeRegistry;

	struct RandomValueOptions
	{
		uint32_t MaxArrayLength = 4;
		uint32_t MaxMapKeys = 8; // Map fields get 0..MaxMapKeys random keys (§5.4)
		uint32_t MaxStringLength = 12;
		// Schemas for Variant resolution (§15.2 "with the resolver fed a fixture schema").
		const IFieldSchemaSource* Schemas = nullptr;
		// The keys drawn (without repetition) for Map fields whose values are Variants, so random values exercise
		// resolution; random identifiers when empty.
		std::vector<std::string> VariantMapKeys;
	};

	// Seeded random in-range values for reflected types (Architecture §5.4: the registry suite's 200 random round trips per
	// type). Every field value it produces satisfies the field's metadata: finite floats within Min/Max (a default range of
	// [-1000, 1000] when unbounded) and outside (-MinMagnitude, MinMagnitude); unit quaternions; enum values from the
	// EnumInfo; valid random UUIDs or null for references; printable ASCII strings; Variant values generated from the
	// resolved schema, or small free-form JSON (null, booleans, finite numbers, strings, arrays and objects up to depth 3)
	// when the field has no resolver or resolution fails. Each struct object is then passed through its type's Generate
	// hooks (StructInfo::Generate), so every object it produces passes StructInfo::Validate and therefore FromJson, not
	// only the per-field checks. Read-only and virtual fields are skipped. The output is a pure function of the seed and
	// the call sequence, identical on every platform and configuration. Not thread-safe (one owner).
	class RandomValueGenerator
	{
	public:
		RandomValueGenerator(const TypeRegistry& registry, uint64_t seed, RandomValueOptions options = {});

		// Assigns a random value to every stored, serialized, writable field of `object` (an instance of `type`), nested
		// structs, array elements and map values included, then runs the Generate hooks bottom-up: each nested struct
		// object's hooks right after its own fields, `type`'s hooks last. The result passes type.Validate.
		void Randomize(const StructInfo& type, void* object);

		// A random JSON value valid for `field` (Variant values through `context`; struct values built with Randomize, so
		// they pass their type's validators), in the §6 spelling.
		[[nodiscard]] Json RandomJson(const FieldInfo& field, const ResolveContext& context);

		[[nodiscard]] Random& GetRandom() { return m_Random; }
	private:
		const TypeRegistry* m_Registry = nullptr; // back-reference; outlives the generator
		RandomValueOptions m_Options;
		Random m_Random;
	};

}
