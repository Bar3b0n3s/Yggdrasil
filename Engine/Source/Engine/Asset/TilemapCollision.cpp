#include "EnginePCH.h"
#include "Engine/Asset/TilemapCollision.h"

// The M21 contract's stubs (Docs/Decisions/0032-m21-contract.md decision 25): stream T2 implements the merged boxes.

namespace Engine {

	bool IsTilemapCellSolid(const TilemapData& /*tilemap*/, uint32_t /*tile*/)
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	std::vector<TilemapBox> MergeTilemapChunkBoxes(const TilemapData& /*tilemap*/, const TilemapChunk& /*chunk*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	std::vector<TilemapBox> MergeTilemapBoxes(const TilemapData& /*tilemap*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
