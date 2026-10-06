#include "EnginePCH.h"
#include "Engine/Reflection/RandomValueGenerator.h"

#include "Engine/Reflection/StructInfo.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// M3 contract stub (Roadmap rule 3): stream A (Reflection) implements seeded random values. The constructor is
// complete.

namespace Engine {

	RandomValueGenerator::RandomValueGenerator(const TypeRegistry& registry, uint64_t seed, RandomValueOptions options)
		: m_Registry(&registry), m_Options(std::move(options)), m_Random(seed)
	{
	}

	void RandomValueGenerator::Randomize(const StructInfo& /*type*/, void* /*object*/)
	{
		ENGINE_CONTRACT_STUB();
		// Keeps Clang's -Wunused-private-field quiet until the implementation reads them.
		static_cast<void>(m_Registry);
		static_cast<void>(m_Options);
	}

	Json RandomValueGenerator::RandomJson(const FieldInfo& /*field*/, const ResolveContext& /*context*/)
	{
		ENGINE_CONTRACT_STUB();
		return Json();
	}

}
