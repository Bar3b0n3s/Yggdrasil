#include "EnginePCH.h"
#include "Engine/Asset/TerrainGeometry.h"

// The terrain's surface (TerrainGeometry.h): contract stubs of M26's C26.1, implemented by stream T-A
// (Docs/Decisions/0037-m26-contract.md decision 21).

namespace Engine {

	glm::vec3 GetTerrainSamplePosition(const HeightfieldData& /*heightfield*/, const TerrainExtent& /*extent*/, uint32_t /*x*/, uint32_t /*z*/)
	{
		ENGINE_CONTRACT_STUB();
		return glm::vec3(0.0f);
	}

	std::optional<float> SampleTerrainHeight(const HeightfieldData& /*heightfield*/, const TerrainExtent& /*extent*/, glm::vec2 /*localXZ*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::optional<glm::vec3> SampleTerrainNormal(const HeightfieldData& /*heightfield*/, const TerrainExtent& /*extent*/, glm::vec2 /*localXZ*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

}
