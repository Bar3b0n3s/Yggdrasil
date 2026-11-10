#include "TestsPCH.h"

#include "Engine/Asset/TilemapData.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Core/BinaryWriter.h"
#include "Support/Fuzz.h"
#include "Support/SceneTestFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <bit>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Tilemaps (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 23): the chunk and cell arithmetic, the format,
// the cooked payload and its fuzz target, the cell edits and the palette checks.

namespace Engine {

	namespace {

		const AssetHandle GroundSheet(0x5b5e000000000002ull);
		const AssetHandle DecorSheet(0x5b5e000000000003ull);

		// Chunks of 2 x 2 cells: chunk (-1, 0) holds cell (-1, 0); chunk (0, 0) a ground row (0, 0) and (1, 0) under a decor cell
		// (0, 1); chunk (0, 1) cell (1, 3).
		TilemapData MakeGroundTilemap()
		{
			TilemapData tilemap;
			tilemap.CellSize = glm::vec2(1.0f, 0.5f);
			tilemap.ChunkSize = 2;
			tilemap.Palette = {
				TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(GroundSheet), .Frame = 0, .Solid = true },
				TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(DecorSheet), .Frame = 4, .Solid = false },
			};
			tilemap.Chunks = {
				TilemapChunk{ .X = -1, .Y = 0, .Tiles = { 0, 1, 0, 0 } },
				TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 1, 2, 0 } },
				TilemapChunk{ .X = 0, .Y = 1, .Tiles = { 0, 0, 0, 1 } },
			};
			return tilemap;
		}

		// The first issue's JSON pointer of a Validation error.
		std::string FirstPointer(const Error& error)
		{
			if (!error.GetIssues().empty())
				return error.GetIssues().front().JsonPointer;
			return error.GetLocation().JsonPointer.value_or(std::string());
		}

		// Fails unless the ground tilemap with `change` applied is a Validation error whose first issue is at `pointer`.
		template<typename Change>
		void CheckInvalidTilemap(std::string_view pointer, Change&& change)
		{
			CAPTURE(pointer);
			TilemapData tilemap = MakeGroundTilemap();
			change(tilemap);
			const Status status = ValidateTilemap(tilemap);
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(FirstPointer(status.error()) == pointer);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("TilemapData: cells fall in floored chunks and tiles centre on their cells")
		{
			// Implemented by the contract: every stream places cells with these.
			CHECK(GetTilemapChunkCoordinate(glm::ivec2(0, 0), 16) == glm::ivec2(0, 0));
			CHECK(GetTilemapChunkCoordinate(glm::ivec2(15, 16), 16) == glm::ivec2(0, 1));
			CHECK(GetTilemapChunkCoordinate(glm::ivec2(-1, -16), 16) == glm::ivec2(-1, -1));
			CHECK(GetTilemapChunkCoordinate(glm::ivec2(-17, -1), 16) == glm::ivec2(-2, -1));
			CHECK(GetTilemapChunkCoordinate(glm::ivec2(-3, 5), 1) == glm::ivec2(-3, 5));
			const TilemapData tilemap = MakeGroundTilemap();
			CHECK(GetTilemapCellCenter(tilemap, glm::ivec2(0, 0)) == glm::vec2(0.5f, 0.25f));
			CHECK(GetTilemapCellCenter(tilemap, glm::ivec2(-1, 3)) == glm::vec2(-0.5f, 1.75f));
		}

		TEST_CASE("TilemapData: a .tilemap file round-trips canonically and its rules are located")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			// The defaults: a unit grid of 16-cell chunks, no palette, no chunks, every field written.
			const Result<std::string> empty = TilemapToText(TilemapData{}, *registry);
			REQUIRE_MESSAGE(empty.has_value(), empty.error().ToString());
			CHECK(*empty
				== "{\n\t\"Format\": \"Tilemap\",\n\t\"Version\": 1,\n\t\"CellSize\": [1, 1],\n\t\"ChunkSize\": 16,\n\t\"Palette\": [],\n\t\"Chunks\": []\n}\n");

			const Result<std::string> text = TilemapToText(MakeGroundTilemap(), *registry);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			// Each chunk's cells are one line of integers (JsonWriter's arrays of scalars).
			CHECK(text->find("\"Tiles\": [1, 1, 2, 0]") != std::string::npos);
			TilemapLoadReport report;
			const Result<TilemapData> read = TilemapFromText(*text, *registry, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(report.FileVersion == TilemapFormatVersion);
			CHECK(report.Diagnostics.empty());
			const TilemapData expected = MakeGroundTilemap();
			CHECK(read->CellSize == expected.CellSize);
			CHECK(read->ChunkSize == expected.ChunkSize);
			CHECK(read->Palette == expected.Palette);
			CHECK(read->Chunks == expected.Chunks);
			CHECK(TilemapToText(*read, *registry).value_or(std::string()) == *text);

			// An unknown member warns, or fails a strict read (the importer's).
			std::string unknown(*text);
			unknown.insert(unknown.find("\"CellSize\""), "\"Layers\": 2,\n\t");
			TilemapLoadReport lenient;
			CHECK(TilemapFromText(unknown, *registry, lenient).has_value());
			CHECK(lenient.Diagnostics.size() == 1);
			TilemapLoadReport strict;
			CHECK_FALSE(TilemapFromText(unknown, *registry, strict, true).has_value());

			// Every rule, located in the document.
			CHECK(ValidateTilemap(MakeGroundTilemap()).has_value());
			CheckInvalidTilemap("/CellSize", [](TilemapData& tilemap)
			{
				tilemap.CellSize.x = 0.0f;
			});
			CheckInvalidTilemap("/ChunkSize", [](TilemapData& tilemap)
			{
				tilemap.ChunkSize = MaxTilemapChunkSize + 1;
			});
			CheckInvalidTilemap("/ChunkSize", [](TilemapData& tilemap)
			{
				tilemap.ChunkSize = 3; // not a power of two: it does not divide 2^20
			});
			CheckInvalidTilemap("/Palette/1/Frame", [](TilemapData& tilemap)
			{
				tilemap.Palette[1].Frame = MaxSpriteSheetFrames;
			});
			CheckInvalidTilemap("/Chunks/1/Tiles/2", [](TilemapData& tilemap)
			{
				tilemap.Chunks[1].Tiles[2] = 3; // beyond the palette's two entries
			});
			CheckInvalidTilemap("/Chunks/1/Tiles", [](TilemapData& tilemap)
			{
				tilemap.Chunks[1].Tiles.pop_back(); // not ChunkSize^2 values
			});
			CheckInvalidTilemap("/Chunks/1", [](TilemapData& tilemap)
			{
				std::swap(tilemap.Chunks[0], tilemap.Chunks[1]); // not sorted by (Y, X)
			});
			CheckInvalidTilemap("/Chunks/1", [](TilemapData& tilemap)
			{
				tilemap.Chunks[1].X = -1; // a duplicate of chunk (-1, 0)
			});
			CheckInvalidTilemap("/Chunks/2", [](TilemapData& tilemap)
			{
				tilemap.Chunks[2].Tiles = { 0, 0, 0, 0 }; // an empty chunk is not stored
			});
			CheckInvalidTilemap("/Chunks/2", [](TilemapData& tilemap)
			{
				tilemap.Chunks[2].Y = MaxTilemapCellCoordinate; // its cells beyond the coordinate range
			});
			// The chunk coordinates' edge at ChunkSize 2: chunk row 2^19 - 1 ends with cell row 2^20 - 1, the last in range, and
			// chunk row 2^19 starts beyond it.
			TilemapData edge = MakeGroundTilemap();
			edge.Chunks[2].Y = MaxTilemapCellCoordinate / 2 - 1;
			CHECK(ValidateTilemap(edge).has_value());
			CheckInvalidTilemap("/Chunks/2", [](TilemapData& tilemap)
			{
				tilemap.Chunks[2].Y = MaxTilemapCellCoordinate / 2;
			});
		}

		TEST_CASE("TilemapData: the cooked payload is the documented little-endian layout")
		{
			// The bytes TilemapData.h documents, written here independently of the serializer, so the Debug and Release unit runs
			// both prove that a tilemap cooks to the same bytes in every configuration.
			const TilemapData tilemap = MakeGroundTilemap();
			BinaryWriter expected;
			expected.WriteU32(std::bit_cast<uint32_t>(tilemap.CellSize.x));
			expected.WriteU32(std::bit_cast<uint32_t>(tilemap.CellSize.y));
			expected.WriteU32(2);
			expected.WriteU32(2);
			expected.WriteU32(3);
			for (const TilemapTile& tile : tilemap.Palette)
			{
				expected.WriteU64(tile.Sheet.GetHandle().GetValue());
				expected.WriteU32(tile.Frame);
				expected.WriteU8(tile.Solid ? 1 : 0);
			}
			for (const TilemapChunk& chunk : tilemap.Chunks)
			{
				expected.WriteI32(chunk.X);
				expected.WriteI32(chunk.Y);
				for (const uint32_t cell : chunk.Tiles)
					expected.WriteU16(static_cast<uint16_t>(cell));
			}
			const Buffer payload = SerializeTilemapPayload(tilemap);
			CHECK(payload == expected.TakeBuffer());

			const Buffer cooked = CookTilemap(tilemap, 1);
			const Result<CookedArtifactView> view = ReadCookedArtifact(cooked, AssetType::Tilemap, TilemapData::FormatVersion);
			REQUIRE_MESSAGE(view.has_value(), view.error().ToString());
			const Result<AssetRef<TilemapData>> loaded = LoadCookedTilemap(cooked);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			CHECK((*loaded)->GetAssetType() == AssetType::Tilemap);
			CHECK((*loaded)->Chunks == tilemap.Chunks);
			CHECK((*loaded)->Palette == tilemap.Palette);

			// Never trusted: a truncated payload, trailing bytes and a Solid byte other than 0 or 1 are Parse errors.
			Buffer truncated = payload;
			truncated.pop_back();
			CHECK(DeserializeTilemapPayload(truncated).error().GetCode() == ErrorCode::Parse);
			Buffer trailing = payload;
			trailing.push_back(std::byte{ 0 });
			CHECK(DeserializeTilemapPayload(trailing).error().GetCode() == ErrorCode::Parse);
			Buffer badSolid = payload;
			badSolid[20 + 8 + 4] = std::byte{ 2 }; // the first palette entry's Solid byte
			CHECK(DeserializeTilemapPayload(badSolid).error().GetCode() == ErrorCode::Parse);
		}

		TEST_CASE("TilemapData: 1,000 seeded mutations of a tilemap file and of its cooked form never crash" * doctest::test_suite(Test::FuzzSuite))
		{
			// The seed corpus: the ground tilemap's text and payload. The text goes through TilemapFromText, the cooked form (the
			// payload hash sealed again) through LoadCookedTilemap; a tilemap either accepts is valid and writes back.
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const Result<std::string> seedText = TilemapToText(MakeGroundTilemap(), *registry);
			REQUIRE_MESSAGE(seedText.has_value(), seedText.error().ToString());
			const Buffer payload = SerializeTilemapPayload(MakeGroundTilemap());
			REQUIRE_FALSE(payload.empty());
			const Test::FuzzRun run = Test::RunFuzzCase(0x7113F022, 1000, [&registry, &seedText, &payload](Random& random, uint64_t /*index*/)
			{
				if (random.NextBool())
				{
					std::string mutated = *seedText;
					Test::MutateText(mutated, random);
					TilemapLoadReport report;
					const Result<TilemapData> tilemap = TilemapFromText(mutated, *registry, report, random.NextBool());
					if (!tilemap.has_value())
						return false;
					CHECK(ValidateTilemap(*tilemap).has_value());
					CHECK(TilemapToText(*tilemap, *registry).has_value());
					return true;
				}
				std::vector<std::byte> mutated(payload.begin(), payload.end());
				Test::MutateBytes(mutated, random);
				const Result<AssetRef<TilemapData>> loaded = LoadCookedTilemap(WriteCookedArtifact(AssetType::Tilemap, TilemapData::FormatVersion, 1, mutated));
				if (!loaded.has_value())
					return false;
				CHECK(ValidateTilemap(**loaded).has_value());
				// Every accepted payload is canonical: it writes back byte for byte.
				CHECK(SerializeTilemapPayload(**loaded) == mutated);
				return true;
			});
			if (Test::IsFuzzSample(run.Range))
			{
				CHECK(run.Counts.Rejected > 0);
				CHECK(run.Counts.Accepted > 0);
			}
		}

		TEST_CASE("TilemapData: cells are read and written by chunk, and an emptied chunk is removed")
		{
			TilemapData tilemap = MakeGroundTilemap();
			CHECK(GetTilemapCell(tilemap, glm::ivec2(-1, 0)) == 1);
			CHECK(GetTilemapCell(tilemap, glm::ivec2(0, 1)) == 2);
			CHECK(GetTilemapCell(tilemap, glm::ivec2(1, 3)) == 1);
			CHECK(GetTilemapCell(tilemap, glm::ivec2(40, -40)) == 0);

			// Painting a far cell creates its chunk at its sorted place; erasing chunk (0, 1)'s only cell removes it; a cell
			// written twice back to its value counts no change; the later edit of a cell wins.
			const std::vector<TilemapCellEdit> edits = {
				{ .Cell = glm::ivec2(5, -3), .Tile = 2 },
				{ .Cell = glm::ivec2(1, 3), .Tile = 0 },
				{ .Cell = glm::ivec2(0, 0), .Tile = 2 },
				{ .Cell = glm::ivec2(0, 0), .Tile = 1 },
				{ .Cell = glm::ivec2(1, 1), .Tile = 1 },
				{ .Cell = glm::ivec2(1, 1), .Tile = 2 },
			};
			const Result<uint32_t> changed = ApplyTilemapCellEdits(tilemap, edits);
			REQUIRE_MESSAGE(changed.has_value(), changed.error().ToString());
			CHECK(*changed == 3);
			CHECK(ValidateTilemap(tilemap).has_value());
			REQUIRE(tilemap.Chunks.size() == 3);
			CHECK(glm::ivec2(tilemap.Chunks[0].X, tilemap.Chunks[0].Y) == glm::ivec2(2, -2));
			CHECK(tilemap.Chunks[0].Tiles == std::vector<uint32_t>{ 0, 0, 0, 2 });
			CHECK(glm::ivec2(tilemap.Chunks[1].X, tilemap.Chunks[1].Y) == glm::ivec2(-1, 0));
			CHECK(tilemap.Chunks[2].Tiles == std::vector<uint32_t>{ 1, 1, 2, 2 });

			// Atomic: an out-of-range cell or a value beyond the palette changes nothing.
			const TilemapData before = tilemap;
			const std::vector<TilemapCellEdit> beyond = { { .Cell = glm::ivec2(0, 0), .Tile = 0 }, { .Cell = glm::ivec2(1, 0), .Tile = 3 } };
			const Result<uint32_t> refused = ApplyTilemapCellEdits(tilemap, beyond);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(tilemap.Chunks == before.Chunks);
			const std::vector<TilemapCellEdit> outside = { { .Cell = glm::ivec2(MaxTilemapCellCoordinate, 0), .Tile = 1 } };
			CHECK(ApplyTilemapCellEdits(tilemap, outside).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(tilemap.Chunks == before.Chunks);

			// The edge cells of the coordinate range lie in chunks ValidateTilemap accepts, whatever the chunk size: chunk sizes
			// are powers of two, which divide 2^20 (decision 23).
			for (const uint32_t chunkSize : { 1U, 2U, MaxTilemapChunkSize })
			{
				CAPTURE(chunkSize);
				TilemapData edge;
				edge.ChunkSize = chunkSize;
				edge.Palette = { TilemapTile{} };
				const glm::ivec2 low(-MaxTilemapCellCoordinate);
				const glm::ivec2 high(MaxTilemapCellCoordinate - 1);
				const std::vector<TilemapCellEdit> corners = { { .Cell = low, .Tile = 1 }, { .Cell = high, .Tile = 1 } };
				const Result<uint32_t> painted = ApplyTilemapCellEdits(edge, corners);
				REQUIRE_MESSAGE(painted.has_value(), painted.error().ToString());
				CHECK(*painted == 2);
				CHECK(ValidateTilemap(edge).has_value());
				REQUIRE(edge.Chunks.size() == 2);
				const int32_t chunkLimit = MaxTilemapCellCoordinate / static_cast<int32_t>(chunkSize);
				CHECK(glm::ivec2(edge.Chunks[0].X, edge.Chunks[0].Y) == glm::ivec2(-chunkLimit));
				CHECK(glm::ivec2(edge.Chunks[1].X, edge.Chunks[1].Y) == glm::ivec2(chunkLimit - 1));
				CHECK(GetTilemapCell(edge, low) == 1);
				CHECK(GetTilemapCell(edge, high) == 1);
			}
		}

		TEST_CASE("TilemapData: the palette's sheets are its dependencies, and frames their sheets lack are found")
		{
			TilemapData tilemap = MakeGroundTilemap();
			tilemap.Palette.push_back(TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(GroundSheet), .Frame = 7 });
			tilemap.Palette.push_back(TilemapTile{});
			CHECK(GetTilemapDependencies(tilemap) == std::vector<AssetHandle>{ GroundSheet, DecorSheet });
			CHECK(GetTilemapDependencies(TilemapData{}).empty());

			// The ground sheet has 2 frames, the decor sheet is not registered (ASSET_MISSING's), the last entry has no sheet:
			// only entry 2's frame 7 is out of range.
			Test::InMemoryAssetManager assets;
			Ref<SpriteSheetData> ground = CreateRef<SpriteSheetData>();
			ground->Frames.resize(2);
			assets.Publish(GroundSheet, ground);
			const std::vector<TilemapPaletteIssue> issues = FindTilemapPaletteIssues(tilemap, assets);
			REQUIRE(issues.size() == 1);
			CHECK(issues[0] == TilemapPaletteIssue{ .Entry = 2, .Sheet = GroundSheet, .Frame = 7, .FrameCount = 2 });
		}
	}

}
