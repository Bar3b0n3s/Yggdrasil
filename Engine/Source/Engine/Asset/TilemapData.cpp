#include "EnginePCH.h"
#include "Engine/Asset/TilemapData.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/BinaryWriter.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <format>
#include <map>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		// The oldest readable .tilemap version (there has been no other yet).
		static constexpr uint32_t MinimumTilemapVersion = 1;
		// The payload bytes of one palette entry (sheet, frame, solid) and of one chunk without its cells (X, Y); a cell is one
		// uint16. DeserializeTilemapPayload checks every count against these before it allocates.
		static constexpr uint64_t PaletteEntryPayloadBytes = 8 + 4 + 1;
		static constexpr uint64_t ChunkHeaderPayloadBytes = 4 + 4;
		static constexpr uint64_t CellPayloadBytes = 2;

		// floor(value / divisor) for a positive divisor, in integers.
		static int32_t FloorDivide(int32_t value, int32_t divisor)
		{
			const int32_t quotient = value / divisor;
			return value % divisor != 0 && value < 0 ? quotient - 1 : quotient;
		}

		// Whether `chunkSize` is a chunk size of the format: a power of two from 1 to MaxTilemapChunkSize.
		[[nodiscard]] static bool IsTilemapChunkSize(uint32_t chunkSize)
		{
			return chunkSize >= 1 && chunkSize <= MaxTilemapChunkSize && std::has_single_bit(chunkSize);
		}

		// Whether chunk `left` sorts before chunk `right` in the format's order, by (Y, X).
		[[nodiscard]] static bool IsChunkBefore(int32_t leftX, int32_t leftY, int32_t rightX, int32_t rightY)
		{
			return leftY != rightY ? leftY < rightY : leftX < rightX;
		}

		// An Error at <context's pointer><relative> (`relative` empty or starting with '/').
		static void AddTilemapIssue(ValidationContext& context, std::string_view relative, std::string message, std::string hint = {})
		{
			ValidationIssue issue;
			issue.Severity = DiagnosticSeverity::Error;
			issue.JsonPointer = context.GetPointer() + std::string(relative);
			issue.Message = std::move(message);
			issue.Hint = std::move(hint);
			context.AddIssue(std::move(issue));
		}

		// One chunk's rules below "/Chunks/<index>", given the tilemap's palette size and a valid chunk size (`chunkSize` 0 when
		// the tilemap's is not one: the rules that depend on it are then left out, its own issue reported at /ChunkSize).
		static void ValidateChunk(const TilemapChunk& chunk, size_t index, uint32_t chunkSize, size_t paletteSize, ValidationContext& context)
		{
			const std::string base = std::format("/Chunks/{}", index);
			if (chunkSize != 0)
			{
				// Compared as they are, before any product with the chunk size, so a fuzzed coordinate never overflows.
				const int32_t limit = MaxTilemapCellCoordinate / static_cast<int32_t>(chunkSize);
				if (chunk.X < -limit || chunk.X >= limit || chunk.Y < -limit || chunk.Y >= limit)
				{
					AddTilemapIssue(context, base,
						std::format("chunk ({}, {}) holds cells outside the coordinate range: at a chunk size of {} a chunk's X and Y lie in [{}, {})", chunk.X, chunk.Y,
							chunkSize, -limit, limit),
						std::format("a cell's x and y lie in [-{}, {})", MaxTilemapCellCoordinate, MaxTilemapCellCoordinate));
				}
				const uint64_t cells = static_cast<uint64_t>(chunkSize) * chunkSize;
				if (chunk.Tiles.size() != cells)
				{
					AddTilemapIssue(context, base + "/Tiles",
						std::format("chunk ({}, {}) holds {} cell values instead of {} ({} x {})", chunk.X, chunk.Y, chunk.Tiles.size(), cells, chunkSize, chunkSize),
						"Tiles lists every cell of the chunk, row by row from its bottom row up, each row left to right");
					return;
				}
			}
			bool empty = true;
			for (size_t cell = 0; cell < chunk.Tiles.size(); ++cell)
			{
				const uint32_t value = chunk.Tiles[cell];
				if (value > paletteSize)
				{
					AddTilemapIssue(context, std::format("{}/Tiles/{}", base, cell),
						std::format("cell value {} names palette entry {}, but the palette has {} entries", value, value, paletteSize),
						"a cell value is 0 for an empty cell or k for Palette[k - 1]");
				}
				empty = empty && value == 0;
			}
			if (empty)
			{
				AddTilemapIssue(context, base, std::format("chunk ({}, {}) holds no tile", chunk.X, chunk.Y),
					"a chunk is stored only while one of its cells is not empty: remove it");
			}
		}

		// Every rule of ValidateTilemap, located below the context's pointer.
		static void ValidateTilemapRules(const TilemapData& tilemap, ValidationContext& context)
		{
			const glm::vec2 cellSize = tilemap.CellSize;
			const auto isCellSize = [](float value)
			{
				return std::isfinite(value) && value >= MinTilemapCellSize && value <= MaxTilemapCellSize;
			};
			if (!isCellSize(cellSize.x) || !isCellSize(cellSize.y))
			{
				AddTilemapIssue(context, "/CellSize", std::format("the cell size ({}, {}) is not a size of the format", cellSize.x, cellSize.y),
					std::format("each component is a finite number of local units from {} to {}", MinTilemapCellSize, MaxTilemapCellSize));
			}
			const bool chunkSizeValid = IsTilemapChunkSize(tilemap.ChunkSize);
			if (!chunkSizeValid)
			{
				AddTilemapIssue(context, "/ChunkSize", std::format("the chunk size {} is not a power of two from 1 to {}", tilemap.ChunkSize, MaxTilemapChunkSize),
					"a chunk holds 1, 2, 4, 8, 16, 32 or 64 cells a side");
			}

			if (tilemap.Palette.size() > MaxTilemapPaletteEntries)
			{
				AddTilemapIssue(context, "/Palette",
					std::format("the palette has {} entries, more than the {} a tilemap may have", tilemap.Palette.size(), MaxTilemapPaletteEntries));
			}
			const size_t entries = std::min<size_t>(tilemap.Palette.size(), MaxTilemapPaletteEntries);
			for (size_t index = 0; index < entries; ++index)
			{
				const uint32_t frame = tilemap.Palette[index].Frame;
				if (frame >= MaxSpriteSheetFrames)
				{
					AddTilemapIssue(context, std::format("/Palette/{}/Frame", index),
						std::format("palette entry {} names frame {}, beyond the {} frames a sprite sheet may have", index, frame, MaxSpriteSheetFrames),
						"a Frame is an index into its sheet's Frames, from 0");
				}
			}

			const uint32_t chunkSize = chunkSizeValid ? tilemap.ChunkSize : 0;
			if (chunkSizeValid && static_cast<uint64_t>(tilemap.Chunks.size()) * chunkSize * chunkSize > MaxTilemapCells)
			{
				AddTilemapIssue(context, "/Chunks",
					std::format("the {} chunks of {} x {} cells store {} cells, more than the {} a tilemap may have", tilemap.Chunks.size(), chunkSize, chunkSize,
						static_cast<uint64_t>(tilemap.Chunks.size()) * chunkSize * chunkSize, MaxTilemapCells),
					"use a smaller chunk size, or split the map");
				return;
			}
			for (size_t index = 0; index < tilemap.Chunks.size(); ++index)
			{
				const TilemapChunk& chunk = tilemap.Chunks[index];
				if (index > 0)
				{
					const TilemapChunk& previous = tilemap.Chunks[index - 1];
					if (!IsChunkBefore(previous.X, previous.Y, chunk.X, chunk.Y))
					{
						const bool duplicate = previous.X == chunk.X && previous.Y == chunk.Y;
						AddTilemapIssue(context, std::format("/Chunks/{}", index),
							duplicate ? std::format("chunk ({}, {}) is listed twice", chunk.X, chunk.Y)
									  : std::format("chunk ({}, {}) is listed after chunk ({}, {})", chunk.X, chunk.Y, previous.X, previous.Y),
							"chunks are listed once each, sorted by Y, then X");
					}
				}
				ValidateChunk(chunk, index, chunkSize, tilemap.Palette.size(), context);
			}
		}

		// The "Tilemap" Validate hook: ValidateTilemap on the whole tilemap. The registry runs it only when the fields read
		// without an issue, so it adds the rules across them.
		static void ValidateTilemapHook(const TilemapData& tilemap, ValidationContext& context)
		{
			ValidateTilemapRules(tilemap, context);
		}

		// The "Tilemap" Generate hook: a chunk size of the format, every chunk's coordinates inside its range, sorted and unique,
		// ChunkSize^2 values of at most the palette's size each and not all 0 (a chunk dropped when the palette is empty).
		static void GenerateTilemap(TilemapData& tilemap, Random& /*random*/)
		{
			tilemap.ChunkSize = std::bit_floor(std::clamp(tilemap.ChunkSize, 1U, MaxTilemapChunkSize));
			const int64_t limit = MaxTilemapCellCoordinate / static_cast<int32_t>(tilemap.ChunkSize);
			const auto wrap = [limit](int32_t value)
			{
				const int64_t span = 2 * limit;
				return static_cast<int32_t>(((static_cast<int64_t>(value) % span) + span) % span - limit);
			};
			const size_t cells = static_cast<size_t>(tilemap.ChunkSize) * tilemap.ChunkSize;
			const uint32_t values = static_cast<uint32_t>(tilemap.Palette.size()) + 1;
			for (TilemapChunk& chunk : tilemap.Chunks)
			{
				chunk.X = wrap(chunk.X);
				chunk.Y = wrap(chunk.Y);
				chunk.Tiles.resize(cells, 0);
				for (uint32_t& value : chunk.Tiles)
					value %= values;
				if (std::ranges::all_of(chunk.Tiles, [](uint32_t value)
				{
					return value == 0;
				}) && values > 1)
					chunk.Tiles.front() = 1;
			}
			std::erase_if(tilemap.Chunks, [](const TilemapChunk& chunk)
			{
				return std::ranges::all_of(chunk.Tiles, [](uint32_t value)
				{
					return value == 0;
				});
			});
			std::ranges::stable_sort(tilemap.Chunks, [](const TilemapChunk& left, const TilemapChunk& right)
			{
				return IsChunkBefore(left.X, left.Y, right.X, right.Y);
			});
			const auto duplicates = std::ranges::unique(tilemap.Chunks, [](const TilemapChunk& left, const TilemapChunk& right)
			{
				return left.X == right.X && left.Y == right.Y;
			});
			tilemap.Chunks.erase(duplicates.begin(), duplicates.end());
		}

		// The registered "Tilemap" struct. A registry without it is a programmer error (asserted); the error return keeps builds
		// without asserts safe.
		[[nodiscard]] static Result<const StructInfo*> FindTilemapType(const TypeRegistry& registry)
		{
			ENGINE_CORE_ASSERT(registry.IsFrozen(), "Tilemaps need a frozen type registry");
			const StructInfo* type = registry.FindStruct<TilemapData>();
			ENGINE_CORE_ASSERT(type != nullptr, "Tilemaps need a registry on which RegisterTilemapTypes ran");
			if (type == nullptr)
				return MakeError(ErrorCode::InvalidState, "the type registry has no Tilemap struct (RegisterTilemapTypes was not called)");
			return type;
		}

		[[nodiscard]] static Result<TilemapData> ReadTilemapDocument(const Json& document, const TypeRegistry& registry, TilemapLoadReport& report,
			bool strictUnknowns)
		{
			ENGINE_TRY_ASSIGN(const StructInfo* type, FindTilemapType(registry));
			const JsonReader root(document);
			ENGINE_TRY_ASSIGN(report.FileVersion, root.ReadFormatHeader(TilemapFormatName, MinimumTilemapVersion, TilemapFormatVersion));

			// Everything below the header is the Tilemap object; the header check above guarantees an object.
			Json fields = document;
			fields.erase("Format");
			fields.erase("Version");

			TilemapData tilemap;
			ReadContext context;
			context.Strict = strictUnknowns;
			context.Diagnostics = &report.Diagnostics;
			ENGINE_TRY(type->FromJson(&tilemap, JsonReader(fields), context));
			ENGINE_TRY(ValidateTilemap(tilemap));
			return tilemap;
		}

		// A payload count `count` (`what`: "palette entries") that must be at most `limit` and leave room for `itemBytes` bytes
		// per item in what follows. Errors: Parse.
		[[nodiscard]] static Status CheckPayloadCount(const BinaryReader& reader, uint64_t count, uint64_t limit, uint64_t itemBytes, std::string_view what)
		{
			if (count > limit)
				return MakeError(ErrorCode::Parse, "tilemap payload: {} {} before offset {}, more than the {} a tilemap may have", count, what, reader.GetPosition(), limit);
			if (count * itemBytes > reader.GetRemaining())
			{
				return MakeError(ErrorCode::Parse, "tilemap payload: {} {} need {} bytes, but {} follow at offset {}", count, what, count * itemBytes, reader.GetRemaining(),
					reader.GetPosition());
			}
			return {};
		}

		// The index in `chunks` (valid: sorted by (Y, X)) of chunk `coordinate`, or nullopt. Logarithmic.
		[[nodiscard]] static std::optional<size_t> FindChunk(std::span<const TilemapChunk> chunks, glm::ivec2 coordinate)
		{
			const auto found = std::ranges::lower_bound(chunks, coordinate, [](glm::ivec2 left, glm::ivec2 right)
			{
				return IsChunkBefore(left.x, left.y, right.x, right.y);
			}, [](const TilemapChunk& chunk)
			{
				return glm::ivec2(chunk.X, chunk.Y);
			});
			if (found == chunks.end() || found->X != coordinate.x || found->Y != coordinate.y)
				return std::nullopt;
			return static_cast<size_t>(found - chunks.begin());
		}

		// The format's chunk order as a comparator of chunk coordinates, by (Y, X).
		struct ChunkCoordinateLess
		{
			[[nodiscard]] bool operator()(glm::ivec2 left, glm::ivec2 right) const
			{
				return IsChunkBefore(left.x, left.y, right.x, right.y);
			}
		};

		// The index of cell `cell` in the Tiles of the chunk at `chunk` (its chunk coordinate) for chunks of `chunkSize`.
		[[nodiscard]] static size_t GetCellIndex(glm::ivec2 cell, glm::ivec2 chunk, uint32_t chunkSize)
		{
			const int64_t size = chunkSize;
			const int64_t column = static_cast<int64_t>(cell.x) - static_cast<int64_t>(chunk.x) * size;
			const int64_t row = static_cast<int64_t>(cell.y) - static_cast<int64_t>(chunk.y) * size;
			return static_cast<size_t>(row * size + column);
		}

	}

	void RegisterTilemapTypes(TypeRegistry& registry)
	{
		// The per-field ranges; the rules across fields (ChunkSize's power of two, the chunks' cells, values, order and
		// coordinates, the counts) are ValidateTilemap's, run as the "Tilemap" Validate hook.
		registry.Struct<TilemapTile>("TilemapTile",
					"One entry of a tilemap's palette: the sprite sheet frame drawn in the cells holding it, and whether the TilemapCollider collides with them.")
			.Field("Sheet", &TilemapTile::Sheet, "The sprite sheet the tile is cut from; null draws nothing.")
			.Field("Frame", &TilemapTile::Frame, "The index of the frame in the sheet's Frames, from 0; a frame the sheet lacks draws nothing (TILEMAP_FRAME_OUT_OF_RANGE).",
				{ .Min = 0.0, .Max = static_cast<double>(MaxSpriteSheetFrames - 1) })
			.Field("Solid", &TilemapTile::Solid, "Whether the TilemapCollider collides with the cells holding this entry.");

		registry.Struct<TilemapChunk>("TilemapChunk",
					"A square of ChunkSize x ChunkSize cells of a tilemap, stored while one of them is not empty; chunk (X, Y) holds the cells from (X ChunkSize, "
					"Y ChunkSize).")
			.Field("X", &TilemapChunk::X,
				std::format("The chunk's column: its cells' x lie in [X ChunkSize, (X + 1) ChunkSize), and in [-{}, {}).", MaxTilemapCellCoordinate, MaxTilemapCellCoordinate))
			.Field("Y", &TilemapChunk::Y,
				std::format("The chunk's row: its cells' y lie in [Y ChunkSize, (Y + 1) ChunkSize), and in [-{}, {}).", MaxTilemapCellCoordinate, MaxTilemapCellCoordinate))
			.Field("Tiles", &TilemapChunk::Tiles,
				"The ChunkSize^2 cell values, row by row from the chunk's bottom row up, each row left to right: 0 for an empty cell, k for Palette[k - 1]; "
				"not all 0.");

		registry.Struct<TilemapData>("Tilemap",
					"A tilemap (.tilemap): a grid of cells in chunks, each cell empty or holding an entry of the palette, a sprite sheet frame drawn by the "
					"TilemapRenderer and collided by the TilemapCollider when Solid.")
			.Field("CellSize", &TilemapData::CellSize, "The size of a cell in the entity's local units: cell (x, y) spans [x CellSize.x, (x + 1) CellSize.x] in X.",
				{ .Min = static_cast<double>(MinTilemapCellSize), .Max = static_cast<double>(MaxTilemapCellSize), .Unit = "units" })
			.Field("ChunkSize", &TilemapData::ChunkSize,
				std::format("The cells a chunk holds a side: a power of two from 1 to {} (16 by default).", MaxTilemapChunkSize),
				{ .Min = 1.0, .Max = static_cast<double>(MaxTilemapChunkSize) })
			.Field("Palette", &TilemapData::Palette, std::format("At most {} entries; a cell value k draws Palette[k - 1].", MaxTilemapPaletteEntries))
			.Field("Chunks", &TilemapData::Chunks,
				std::format("The stored chunks, sorted by Y, then X, each once, none empty; at most {} cells in all.", MaxTilemapCells))
			.Validate(&Utils::ValidateTilemapHook)
			.Generate(&Utils::GenerateTilemap);
	}

	Status ValidateTilemap(const TilemapData& tilemap)
	{
		ValidationContext context;
		Utils::ValidateTilemapRules(tilemap, context);
		return context.ToStatus("the tilemap");
	}

	std::vector<AssetHandle> GetTilemapDependencies(const TilemapData& tilemap)
	{
		std::vector<AssetHandle> handles;
		for (const TilemapTile& tile : tilemap.Palette)
		{
			if (tile.Sheet.GetHandle().IsValid())
				handles.push_back(tile.Sheet.GetHandle());
		}
		std::ranges::sort(handles);
		const auto duplicates = std::ranges::unique(handles);
		handles.erase(duplicates.begin(), duplicates.end());
		return handles;
	}

	Result<Json> TilemapToJson(const TilemapData& tilemap, const TypeRegistry& registry)
	{
		ENGINE_TRY_ASSIGN(const StructInfo* type, Utils::FindTilemapType(registry));
		ENGINE_TRY_ASSIGN(Json fields, type->ToJson(&tilemap));

		// The header first, then the fields in registry order (§6).
		Json document = Json::object();
		document["Format"] = std::string(TilemapFormatName);
		document["Version"] = TilemapFormatVersion;
		for (auto field = fields.begin(); field != fields.end(); ++field)
			document[field.key()] = std::move(*field);
		return document;
	}

	Result<std::string> TilemapToText(const TilemapData& tilemap, const TypeRegistry& registry, JsonStyle style)
	{
		ENGINE_TRY_ASSIGN(const Json document, TilemapToJson(tilemap, registry));
		return JsonWriter::Write(document, style);
	}

	Result<TilemapData> TilemapFromJson(const Json& document, const TypeRegistry& registry, TilemapLoadReport& report, bool strictUnknowns)
	{
		report = TilemapLoadReport{};
		return Utils::ReadTilemapDocument(document, registry, report, strictUnknowns);
	}

	Result<TilemapData> TilemapFromText(std::string_view text, const TypeRegistry& registry, TilemapLoadReport& report, bool strictUnknowns)
	{
		report = TilemapLoadReport{};
		ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
		return TilemapFromJson(document, registry, report, strictUnknowns);
	}

	Buffer SerializeTilemapPayload(const TilemapData& tilemap)
	{
		ENGINE_CORE_ASSERT(ValidateTilemap(tilemap).has_value(), "Only a valid tilemap is cooked");
		BinaryWriter writer;
		writer.WriteF32(tilemap.CellSize.x);
		writer.WriteF32(tilemap.CellSize.y);
		writer.WriteU32(tilemap.ChunkSize);
		writer.WriteU32(static_cast<uint32_t>(tilemap.Palette.size()));
		writer.WriteU32(static_cast<uint32_t>(tilemap.Chunks.size()));
		for (const TilemapTile& tile : tilemap.Palette)
		{
			writer.WriteU64(tile.Sheet.GetHandle().GetValue());
			writer.WriteU32(tile.Frame);
			writer.WriteU8(tile.Solid ? 1 : 0);
		}
		for (const TilemapChunk& chunk : tilemap.Chunks)
		{
			writer.WriteI32(chunk.X);
			writer.WriteI32(chunk.Y);
			for (const uint32_t cell : chunk.Tiles)
				writer.WriteU16(static_cast<uint16_t>(cell));
		}
		return writer.TakeBuffer();
	}

	Result<TilemapData> DeserializeTilemapPayload(std::span<const std::byte> payload)
	{
		BinaryReader reader(payload);
		TilemapData tilemap;
		ENGINE_TRY_ASSIGN(tilemap.CellSize.x, reader.ReadF32());
		ENGINE_TRY_ASSIGN(tilemap.CellSize.y, reader.ReadF32());
		const size_t chunkSizeOffset = reader.GetPosition();
		ENGINE_TRY_ASSIGN(tilemap.ChunkSize, reader.ReadU32());
		if (tilemap.ChunkSize == 0 || tilemap.ChunkSize > MaxTilemapChunkSize)
		{
			return MakeError(ErrorCode::Parse, "tilemap payload: the chunk size at offset {} is {}, not one from 1 to {}", chunkSizeOffset, tilemap.ChunkSize,
				MaxTilemapChunkSize);
		}
		ENGINE_TRY_ASSIGN(const uint32_t paletteCount, reader.ReadU32());
		ENGINE_TRY_ASSIGN(const uint32_t chunkCount, reader.ReadU32());
		const uint64_t cellsPerChunk = static_cast<uint64_t>(tilemap.ChunkSize) * tilemap.ChunkSize;
		const uint64_t chunkBytes = Utils::ChunkHeaderPayloadBytes + cellsPerChunk * Utils::CellPayloadBytes;
		ENGINE_TRY(Utils::CheckPayloadCount(reader, paletteCount, MaxTilemapPaletteEntries, Utils::PaletteEntryPayloadBytes, "palette entries"));
		ENGINE_TRY(Utils::CheckPayloadCount(reader, chunkCount, MaxTilemapCells / cellsPerChunk, chunkBytes, "chunks"));
		const uint64_t needed = static_cast<uint64_t>(paletteCount) * Utils::PaletteEntryPayloadBytes + static_cast<uint64_t>(chunkCount) * chunkBytes;
		if (needed != reader.GetRemaining())
		{
			return MakeError(ErrorCode::Parse, "tilemap payload: {} palette entries and {} chunks take {} bytes, but {} follow at offset {}", paletteCount, chunkCount,
				needed, reader.GetRemaining(), reader.GetPosition());
		}

		tilemap.Palette.reserve(paletteCount);
		for (uint32_t index = 0; index < paletteCount; ++index)
		{
			TilemapTile tile;
			ENGINE_TRY_ASSIGN(const uint64_t sheet, reader.ReadU64());
			tile.Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(sheet));
			ENGINE_TRY_ASSIGN(tile.Frame, reader.ReadU32());
			const size_t solidOffset = reader.GetPosition();
			ENGINE_TRY_ASSIGN(const uint8_t solid, reader.ReadU8());
			if (solid > 1)
				return MakeError(ErrorCode::Parse, "tilemap payload: the Solid byte at offset {} is {} instead of 0 or 1", solidOffset, solid);
			tile.Solid = solid == 1;
			tilemap.Palette.push_back(tile);
		}
		tilemap.Chunks.reserve(chunkCount);
		for (uint32_t index = 0; index < chunkCount; ++index)
		{
			TilemapChunk chunk;
			ENGINE_TRY_ASSIGN(chunk.X, reader.ReadI32());
			ENGINE_TRY_ASSIGN(chunk.Y, reader.ReadI32());
			chunk.Tiles.reserve(static_cast<size_t>(cellsPerChunk));
			for (uint64_t cell = 0; cell < cellsPerChunk; ++cell)
			{
				ENGINE_TRY_ASSIGN(const uint16_t value, reader.ReadU16());
				chunk.Tiles.push_back(value);
			}
			tilemap.Chunks.push_back(std::move(chunk));
		}
		if (!reader.IsAtEnd())
			return MakeError(ErrorCode::Parse, "tilemap payload has {} trailing bytes after offset {}", reader.GetRemaining(), reader.GetPosition());
		ENGINE_TRY(ValidateTilemap(tilemap));
		return tilemap;
	}

	Buffer CookTilemap(const TilemapData& tilemap, uint32_t importerVersion)
	{
		const Buffer payload = SerializeTilemapPayload(tilemap);
		return WriteCookedArtifact(AssetType::Tilemap, TilemapData::FormatVersion, importerVersion, payload);
	}

	Result<AssetRef<TilemapData>> LoadCookedTilemap(std::span<const std::byte> cooked)
	{
		ENGINE_TRY_ASSIGN(const CookedArtifactView view, ReadCookedArtifact(cooked, AssetType::Tilemap, TilemapData::FormatVersion));
		ENGINE_TRY_ASSIGN(TilemapData tilemap, DeserializeTilemapPayload(view.Payload));
		return AssetRef<TilemapData>(CreateRef<TilemapData>(std::move(tilemap)));
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

	uint32_t GetTilemapCell(const TilemapData& tilemap, glm::ivec2 cell)
	{
		if (!Utils::IsTilemapChunkSize(tilemap.ChunkSize))
			return 0;
		const glm::ivec2 coordinate = GetTilemapChunkCoordinate(cell, tilemap.ChunkSize);
		const std::optional<size_t> chunk = Utils::FindChunk(tilemap.Chunks, coordinate);
		if (!chunk.has_value())
			return 0;
		const std::vector<uint32_t>& tiles = tilemap.Chunks[*chunk].Tiles;
		const size_t index = Utils::GetCellIndex(cell, coordinate, tilemap.ChunkSize);
		return index < tiles.size() ? tiles[index] : 0;
	}

	Result<uint32_t> ApplyTilemapCellEdits(TilemapData& tilemap, std::span<const TilemapCellEdit> edits)
	{
		ENGINE_CORE_ASSERT(ValidateTilemap(tilemap).has_value(), "Cell edits apply to a valid tilemap");
		// Every edit is checked first, so a refused call changes nothing.
		for (size_t index = 0; index < edits.size(); ++index)
		{
			const TilemapCellEdit& edit = edits[index];
			const auto inRange = [](int32_t value)
			{
				return value >= -MaxTilemapCellCoordinate && value < MaxTilemapCellCoordinate;
			};
			if (!inRange(edit.Cell.x) || !inRange(edit.Cell.y))
			{
				return MakeError(ErrorCode::InvalidArgument, "cell edit {} writes cell ({}, {}), outside the coordinate range [-{}, {})", index, edit.Cell.x, edit.Cell.y,
					MaxTilemapCellCoordinate, MaxTilemapCellCoordinate);
			}
			if (edit.Tile > tilemap.Palette.size())
			{
				return MakeError(ErrorCode::InvalidArgument, "cell edit {} writes value {} to cell ({}, {}), but the palette has {} entries", index, edit.Tile,
					edit.Cell.x, edit.Cell.y, tilemap.Palette.size());
			}
		}

		// The final value of each edited cell, by chunk (sorted by (Y, X)) and index: a later edit of a cell wins.
		std::map<glm::ivec2, std::map<size_t, uint32_t>, Utils::ChunkCoordinateLess> written;
		for (const TilemapCellEdit& edit : edits)
		{
			const glm::ivec2 coordinate = GetTilemapChunkCoordinate(edit.Cell, tilemap.ChunkSize);
			written[coordinate][Utils::GetCellIndex(edit.Cell, coordinate, tilemap.ChunkSize)] = edit.Tile;
		}

		// The chunks after the edits: the untouched ones as they are and the touched ones rewritten, merged in sorted order, an
		// emptied chunk left out.
		const size_t cellsPerChunk = static_cast<size_t>(tilemap.ChunkSize) * tilemap.ChunkSize;
		std::vector<TilemapChunk> chunks;
		chunks.reserve(tilemap.Chunks.size() + written.size());
		uint32_t changed = 0;
		size_t next = 0;
		const auto copyUntil = [&tilemap, &chunks, &next](const glm::ivec2* stop)
		{
			while (next < tilemap.Chunks.size()
				&& (stop == nullptr || Utils::IsChunkBefore(tilemap.Chunks[next].X, tilemap.Chunks[next].Y, stop->x, stop->y)))
			{
				chunks.push_back(tilemap.Chunks[next]);
				++next;
			}
		};
		for (const auto& [coordinate, cells] : written)
		{
			copyUntil(&coordinate);
			TilemapChunk chunk{ .X = coordinate.x, .Y = coordinate.y, .Tiles = std::vector<uint32_t>(cellsPerChunk, 0) };
			if (next < tilemap.Chunks.size() && tilemap.Chunks[next].X == coordinate.x && tilemap.Chunks[next].Y == coordinate.y)
			{
				chunk.Tiles = tilemap.Chunks[next].Tiles;
				++next;
			}
			for (const auto& [index, value] : cells)
			{
				if (chunk.Tiles[index] != value)
					++changed;
				chunk.Tiles[index] = value;
			}
			if (std::ranges::any_of(chunk.Tiles, [](uint32_t value)
			{
				return value != 0;
			}))
				chunks.push_back(std::move(chunk));
		}
		copyUntil(nullptr);

		const uint64_t stored = static_cast<uint64_t>(chunks.size()) * cellsPerChunk;
		if (stored > MaxTilemapCells)
		{
			ValidationContext context;
			Utils::AddTilemapIssue(context, "/Chunks",
				std::format("the edits would store {} cells in {} chunks, more than the {} a tilemap may have", stored, chunks.size(), MaxTilemapCells),
				"use a smaller chunk size, or split the map");
			const Status refused = context.ToStatus("the tilemap");
			return std::unexpected(refused.error());
		}
		tilemap.Chunks = std::move(chunks);
		return changed;
	}

	std::vector<TilemapPaletteIssue> FindTilemapPaletteIssues(const TilemapData& tilemap, AssetManager& assets)
	{
		// Each sheet loaded once: its frame count, or nullopt for one that is missing, fails to load or is not a sprite sheet.
		std::map<AssetHandle, std::optional<uint32_t>> sheets;
		const auto frameCount = [&assets, &sheets](AssetHandle handle) -> std::optional<uint32_t>
		{
			if (const auto found = sheets.find(handle); found != sheets.end())
				return found->second;
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			const AssetRef<SpriteSheetData> sheet = loaded.has_value() ? AssetCast<SpriteSheetData>(*loaded) : nullptr;
			const std::optional<uint32_t> count = sheet != nullptr ? std::optional<uint32_t>(static_cast<uint32_t>(sheet->Frames.size())) : std::nullopt;
			sheets.emplace(handle, count);
			return count;
		};

		std::vector<TilemapPaletteIssue> issues;
		for (size_t entry = 0; entry < tilemap.Palette.size(); ++entry)
		{
			const TilemapTile& tile = tilemap.Palette[entry];
			const AssetHandle sheet = tile.Sheet.GetHandle();
			if (!sheet.IsValid())
				continue;
			const std::optional<uint32_t> count = frameCount(sheet);
			if (count.has_value() && tile.Frame >= *count)
				issues.push_back(TilemapPaletteIssue{ .Entry = static_cast<uint32_t>(entry), .Sheet = sheet, .Frame = tile.Frame, .FrameCount = *count });
		}
		return issues;
	}

}
