#include "EnginePCH.h"
#include "Engine/Asset/TilemapData.h"

#include "Engine/Core/Assert.h"

#include <nlohmann/json.hpp>

// The M21 contract's tilemap data (Docs/Decisions/0032-m21-contract.md decision 23): the chunk and cell-centre arithmetic every
// stream shares is implemented; stream T1 implements the rest.

namespace Engine {

	namespace Utils {

		// floor(value / divisor) for a positive divisor, in integers.
		static int32_t FloorDivide(int32_t value, int32_t divisor)
		{
			const int32_t quotient = value / divisor;
			return value % divisor != 0 && value < 0 ? quotient - 1 : quotient;
		}

	}

	void RegisterTilemapTypes(TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	Status ValidateTilemap(const TilemapData& /*tilemap*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "tilemap validation (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	std::vector<AssetHandle> GetTilemapDependencies(const TilemapData& /*tilemap*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<Json> TilemapToJson(const TilemapData& /*tilemap*/, const TypeRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the .tilemap writer (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	Result<std::string> TilemapToText(const TilemapData& /*tilemap*/, const TypeRegistry& /*registry*/, JsonStyle /*style*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the .tilemap writer (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	Result<TilemapData> TilemapFromJson(const Json& /*document*/, const TypeRegistry& /*registry*/, TilemapLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the .tilemap reader (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	Result<TilemapData> TilemapFromText(std::string_view /*text*/, const TypeRegistry& /*registry*/, TilemapLoadReport& /*report*/, bool /*strictUnknowns*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the .tilemap reader (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	Buffer SerializeTilemapPayload(const TilemapData& /*tilemap*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<TilemapData> DeserializeTilemapPayload(std::span<const std::byte> /*payload*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the cooked tilemap reader (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	Buffer CookTilemap(const TilemapData& /*tilemap*/, uint32_t /*importerVersion*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<TilemapData>> LoadCookedTilemap(std::span<const std::byte> /*cooked*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "the tilemap loader (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	glm::ivec2 GetTilemapChunkCoordinate(glm::ivec2 cell, uint32_t chunkSize)
	{
		ENGINE_CORE_ASSERT(chunkSize >= 1 && chunkSize <= MaxTilemapChunkSize, "a tilemap chunk holds 1 to {} cells a side, not {}", MaxTilemapChunkSize,
			chunkSize);
		const int32_t size = static_cast<int32_t>(chunkSize);
		return glm::ivec2(Utils::FloorDivide(cell.x, size), Utils::FloorDivide(cell.y, size));
	}

	glm::vec2 GetTilemapCellCenter(const TilemapData& tilemap, glm::ivec2 cell)
	{
		return glm::vec2((static_cast<float>(cell.x) + 0.5f) * tilemap.CellSize.x, (static_cast<float>(cell.y) + 0.5f) * tilemap.CellSize.y);
	}

	uint32_t GetTilemapCell(const TilemapData& /*tilemap*/, glm::ivec2 /*cell*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	Result<uint32_t> ApplyTilemapCellEdits(TilemapData& /*tilemap*/, std::span<const TilemapCellEdit> /*edits*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "tilemap cell edits (Docs/Decisions/0032-m21-contract.md decision 23, stream T1)");
	}

	std::vector<TilemapPaletteIssue> FindTilemapPaletteIssues(const TilemapData& /*tilemap*/, AssetManager& /*assets*/)
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

}
