#include "EditorPCH.h"
#include "EditorCore/Viewport/TilemapPainting.h"

#include <nlohmann/json.hpp>

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decision 26): stream T3 implements the tile brush's helpers.

namespace Engine {

	std::optional<glm::ivec2> IntersectTilemapCell(const RenderRay& /*ray*/, const glm::mat4& /*tilemapWorld*/, const glm::vec2& /*cellSize*/)
	{
		ENGINE_CONTRACT_STUB();
		return std::nullopt;
	}

	std::vector<glm::ivec2> GetTilemapStrokeCells(glm::ivec2 /*from*/, glm::ivec2 /*to*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Json> MakeTilemapPaintParams(const TilemapPaintRequest& /*request*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the tile brush (Docs/Decisions/0032-m21-contract.md decision 26, stream T3)");
	}

}
