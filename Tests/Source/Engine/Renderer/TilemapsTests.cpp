#include "TestsPCH.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/QuadBatcher.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <utility>
#include <vector>

// Tilemaps through the sprite path (M21, C21.2, R-2D-TILE; Docs/Decisions/0032-m21-contract.md decision 24, stream T1; Roadmap
// M21 "A tilemap renders byte-identical to the same tiles drawn as sprites"; plan §7 item 3: every new GPU owner has a loss
// test through the shared helper). The gpu suite runs every case under API 1.4 and again capped at 1.3. The byte-identical
// case's lit variant (lit tiles through stream L's HdrLit pipeline) and the case of a tilemap of more than MaxQuadsPerRecord
// tiles (stream L's batches beyond the QuadBatcher's capacity) need two streams and run from W21.2's integration (decision 33).

namespace Engine {

	namespace {

		// 8 x 8 texels of distinct sRGB values, cut into four 4 x 4 frames: (0, 0), (4, 0), (0, 4), (4, 4).
		constexpr uint32_t TileTexels = 8;
		const AssetHandle TileTextureHandle = UUID(8940);
		const AssetHandle TileSheetHandle = UUID(8941);
		const AssetHandle TilemapHandle = UUID(8942);

		Ref<TextureData> MakeTileTexture()
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = TileTexels;
			texture->Height = TileTexels;
			texture->Mips = { TextureMip{ .Width = TileTexels, .Height = TileTexels, .Offset = 0, .Size = TileTexels * TileTexels * 4 } };
			for (uint32_t y = 0; y < TileTexels; ++y)
			{
				for (uint32_t x = 0; x < TileTexels; ++x)
				{
					texture->Pixels.push_back(static_cast<std::byte>(32 * x));
					texture->Pixels.push_back(static_cast<std::byte>(32 * y));
					texture->Pixels.push_back(static_cast<std::byte>(255 - 16 * x));
					texture->Pixels.push_back(std::byte{ 255 });
				}
			}
			return texture;
		}

		// Frames 0 to 3 are the texture's four 4 x 4 quarters, pivoted at their centres; frame 4 is the texture's
		// `overhangTexels` x `overhangTexels` corner (by default the whole 8 x 8 texture) pivoted at the middle of its right
		// edge, so a tile of it lies left of its cell's centre.
		Ref<SpriteSheetData> MakeTileSheet(uint32_t overhangTexels = TileTexels)
		{
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Texture = TypedAssetHandle<AssetType::Texture>(TileTextureHandle);
			for (const glm::uvec2 origin : { glm::uvec2(0, 0), glm::uvec2(4, 0), glm::uvec2(0, 4), glm::uvec2(4, 4) })
				sheet->Frames.push_back(SpriteFrame{ .Rect = { .X = origin.x, .Y = origin.y, .Width = 4, .Height = 4 } });
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .X = 0, .Y = 0, .Width = overhangTexels, .Height = overhangTexels }, .Pivot = glm::vec2(1.0f, 0.5f) });
			return sheet;
		}

		// Cells of 2 x 2 units (4 texels at 2 pixels per unit: one tile per cell, edge to edge) in chunks of 2 x 2 cells. The
		// cells [-2, 2) x [-2, 2) fill the 8-unit view, with a few left empty; chunk (40, 40) lies far outside it.
		Ref<TilemapData> MakeTilemap(bool withFarChunk)
		{
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->CellSize = glm::vec2(2.0f);
			tilemap->ChunkSize = 2;
			for (uint32_t frame = 0; frame < 4; ++frame)
				tilemap->Palette.push_back(TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(TileSheetHandle), .Frame = frame });
			tilemap->Chunks = {
				TilemapChunk{ .X = -1, .Y = -1, .Tiles = { 1, 2, 0, 3 } },
				TilemapChunk{ .X = 0, .Y = -1, .Tiles = { 4, 4, 1, 0 } },
				TilemapChunk{ .X = -1, .Y = 0, .Tiles = { 2, 2, 3, 1 } },
				TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 0, 1, 2, 3 } },
			};
			if (withFarChunk)
				tilemap->Chunks.push_back(TilemapChunk{ .X = 40, .Y = 40, .Tiles = { 1, 1, 1, 1 } });
			return tilemap;
		}

		// MakeTilemap's cells drawn at 1 pixel per unit, so each 4-texel tile is 4 x 4 units, twice its cell, centred on it:
		// neighbouring tiles overlap, across the chunks' edges too, and the order they draw in shows. Palette entry 5 (frame 4,
		// 8 x 8 units left of its cell's centre) fills cell (4, 0) of chunk (2, 0), whose cells [8, 12] x [0, 4] lie wholly
		// outside the view [-4, 4]^2 while its tile, [1, 9] x [-3, 5], reaches into it.
		constexpr float OverhangPixelsPerUnit = 1.0f;

		Ref<TilemapData> MakeOverhangingTilemap()
		{
			Ref<TilemapData> tilemap = MakeTilemap(false);
			tilemap->Palette.push_back(TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(TileSheetHandle), .Frame = 4 });
			tilemap->Chunks.push_back(TilemapChunk{ .X = 2, .Y = 0, .Tiles = { 5, 0, 0, 0 } });
			return tilemap;
		}

		// The tiles of `cells` (empty cells skipped) as sprites, each with the world matrix of its cell's centre, in that order.
		void AppendTileSprites(RenderSnapshot& snapshot, const TilemapData& tilemap, std::span<const glm::ivec2> cells, const glm::vec4& tint,
			float pixelsPerUnit, bool lit = false)
		{
			for (const glm::ivec2 cell : cells)
			{
				const uint32_t value = GetTilemapCell(tilemap, cell);
				if (value == 0)
					continue;
				const glm::vec2 center = GetTilemapCellCenter(tilemap, cell);
				snapshot.Sprites.push_back(SpriteItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(center, 0.0f)),
					.Sheet = TileSheetHandle,
					.Frame = tilemap.Palette[value - 1].Frame,
					.Color = tint,
					.PixelsPerUnit = pixelsPerUnit,
					.Lit = lit,
					.Entity = UUID(91) });
			}
		}

		// A warm point light over the view's upper left, fading out across it, and a dim global light (C21.2, decision 22): both
		// reach the view, so lit tiles and lit sprites take the same light at each pixel.
		void AddTileLights(RenderSnapshot& snapshot)
		{
			snapshot.Lights2D.push_back(Light2DItem{ .Type = RenderLight2DType::Point,
				.Position = glm::vec3(-1.0f, 1.0f, 0.0f),
				.Color = glm::vec3(1.0f, 0.8f, 0.6f),
				.Intensity = 1.5f,
				.Radius = 3.0f,
				.Falloff = 0.5f,
				.Height = 1.0f,
				.Entity = UUID(92) });
			snapshot.Lights2D.push_back(Light2DItem{ .Type = RenderLight2DType::Global, .Color = glm::vec3(0.1f), .Intensity = 1.0f, .Entity = UUID(93) });
		}

		// A 64 x 64 view of a 16 x 16 pixel-art scene (scale 4) whose orthographic camera at z = 10 shows 8 units: a unit is 2
		// scene pixels and a 4-texel tile at 2 pixels per unit one 4 x 4 block of scene pixels.
		RenderSnapshot MakeTileSnapshot(const Test::SceneRendererIntegrationFixture& setup)
		{
			RenderSnapshot snapshot = setup.Snapshot(RenderProjection::Orthographic);
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			snapshot.Output = RenderOutputSettings{ .SceneWidth = 16, .SceneHeight = 16, .PixelArt = true };
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f));
			snapshot.Camera.Position = glm::vec3(0.0f, 0.0f, 10.0f);
			snapshot.Camera.OrthographicSize = 4.0f;
			snapshot.Camera.ViewportWidth = 16;
			snapshot.Camera.ViewportHeight = 16;
			snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 4.0f, 0.1f, 20.0f, 16, 16);
			return snapshot;
		}

		void PublishTiles(Test::SceneRendererIntegrationFixture& setup, Ref<TilemapData> tilemap)
		{
			setup.Assets().Publish(TileTextureHandle, MakeTileTexture());
			setup.Assets().Publish(TileSheetHandle, MakeTileSheet());
			setup.Assets().Publish(TilemapHandle, std::move(tilemap));
		}

		uint32_t SpriteTriangles(Test::SceneRendererIntegrationFixture& setup)
		{
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "Sprites");
			return pass != nullptr ? pass->Triangles : 0u;
		}

	}

	TEST_SUITE("Renderer")
	{
		// Decision 24's rules, each of which the comparison catches: a tile larger than its cell draws as its sprite; the tiles
		// draw in the tilemap's cell order across the chunks' edges, never chunk by chunk; and a chunk's bounds are its tiles'
		// quads, so chunk (2, 0), outside the view, draws its tile that reaches into it.
		TEST_CASE("Tilemaps: a tilemap renders byte-identical to the same tiles drawn as sprites" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			const Ref<TilemapData> tilemap = MakeOverhangingTilemap();
			PublishTiles(setup, tilemap);
			const glm::vec4 tint(1.0f, 0.75f, 0.5f, 1.0f);

			RenderSnapshot tiles = MakeTileSnapshot(setup);
			tiles.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .Color = tint, .PixelsPerUnit = OverhangPixelsPerUnit, .Entity = UUID(90) });
			const Image tileImage = setup.Render(tiles);
			// The Sprites row draws a tile as a quad, as a sprite: the 13 tiles of the cells in view and chunk (2, 0)'s one.
			CHECK(SpriteTriangles(setup) == 28);

			// The same tiles as sprites, each with the tilemap's world matrix times the translation to its cell's centre, in the
			// tiles' draw order: rows from the top down, each left to right, across every chunk.
			std::vector<glm::ivec2> rows;
			for (int32_t y = 1; y >= -2; --y)
			{
				for (int32_t x = -2; x <= 5; ++x)
					rows.emplace_back(x, y);
			}
			RenderSnapshot sprites = MakeTileSnapshot(setup);
			sprites.FrameIndex += 1;
			AppendTileSprites(sprites, *tilemap, rows, tint, OverhangPixelsPerUnit);
			REQUIRE(sprites.Sprites.size() == 14);
			const Image spriteImage = setup.Render(sprites);
			CHECK(tileImage.Pixels == spriteImage.Pixels);
			// Something was drawn: the clear colour is black, and every texel of the tiles has blue.
			const std::span<const std::byte> row = tileImage.GetRow(4 * 12 + 2);
			CHECK(std::to_integer<int>(row[4 * 2 + 2]) > 0);

			// The comparison sees both rules: drawn chunk by chunk (each chunk's rows from the top down), or without the tile
			// of the chunk outside the view, the same tiles give other images.
			std::vector<glm::ivec2> chunkByChunk;
			for (const TilemapChunk& chunk : tilemap->Chunks)
			{
				for (int32_t y = 1; y >= 0; --y)
				{
					for (int32_t x = 0; x <= 1; ++x)
						chunkByChunk.emplace_back(chunk.X * 2 + x, chunk.Y * 2 + y);
				}
			}
			RenderSnapshot chunked = MakeTileSnapshot(setup);
			chunked.FrameIndex += 2;
			AppendTileSprites(chunked, *tilemap, chunkByChunk, tint, OverhangPixelsPerUnit);
			REQUIRE(chunked.Sprites.size() == 14);
			CHECK(setup.Render(chunked).Pixels != tileImage.Pixels);
			std::vector<glm::ivec2> cellsInView;
			std::ranges::copy_if(rows, std::back_inserter(cellsInView), [](const glm::ivec2 cell)
			{
				return cell.x <= 1;
			});
			RenderSnapshot culledByCells = MakeTileSnapshot(setup);
			culledByCells.FrameIndex += 3;
			AppendTileSprites(culledByCells, *tilemap, cellsInView, tint, OverhangPixelsPerUnit);
			REQUIRE(culledByCells.Sprites.size() == 13);
			CHECK(setup.Render(culledByCells).Pixels != tileImage.Pixels);

			// The lit variant (decision 33: stream T1's tiles through stream L's HdrLit pipeline): the tilemap made Lit, under a
			// point light and a global light, renders byte-identical to the same tiles drawn as lit sprites, through the Lights2D
			// pass, and the lights change the image.
			RenderSnapshot litTiles = MakeTileSnapshot(setup);
			litTiles.FrameIndex += 4;
			litTiles.Tilemaps.push_back(
				TilemapItem{ .Tilemap = TilemapHandle, .Color = tint, .PixelsPerUnit = OverhangPixelsPerUnit, .Lit = true, .Entity = UUID(90) });
			AddTileLights(litTiles);
			const Image litTileImage = setup.Render(litTiles);
			CHECK(SpriteTriangles(setup) == 28);
			const RenderStats litStats = setup.Renderer().GetRenderStats();
			const RenderPassStats* lights = Test::SceneRendererIntegrationFixture::FindPass(litStats, "Lights2D");
			REQUIRE(lights != nullptr);
			CHECK(lights->DrawCalls >= 2);
			RenderSnapshot litSprites = MakeTileSnapshot(setup);
			litSprites.FrameIndex += 5;
			AppendTileSprites(litSprites, *tilemap, rows, tint, OverhangPixelsPerUnit, true);
			REQUIRE(litSprites.Sprites.size() == 14);
			AddTileLights(litSprites);
			CHECK(setup.Render(litSprites).Pixels == litTileImage.Pixels);
			CHECK(litTileImage.Pixels != tileImage.Pixels);
		}

		TEST_CASE("Tilemaps: only the chunks that meet the view are drawn" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			PublishTiles(setup, MakeTilemap(true));
			RenderSnapshot snapshot = MakeTileSnapshot(setup);
			snapshot.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .PixelsPerUnit = 2.0f, .Entity = UUID(90) });
			const Image image = setup.Render(snapshot);
			// Chunk (40, 40) lies some 160 units away: its four tiles are not drawn, the 13 in view are.
			CHECK(SpriteTriangles(setup) == 26);
			// Moving the tilemap so that only that chunk is in view draws its four tiles alone.
			snapshot.Tilemaps.front().World = glm::translate(glm::mat4(1.0f), glm::vec3(-162.0f, -162.0f, 0.0f));
			snapshot.FrameIndex += 1;
			const Image moved = setup.Render(snapshot);
			CHECK(SpriteTriangles(setup) == 8);
			CHECK(moved.Pixels != image.Pixels);
		}

		// Decision 24: the tilemaps and the sprites form one draw order, each tilemap drawn before Sprites[DrawPosition], so a
		// sprite over a tile shows when the tilemap draws before it and the tile when the tilemap draws after it.
		TEST_CASE("Tilemaps: a tilemap draws among the sprites at its DrawPosition" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			PublishTiles(setup, MakeTilemap(false));
			// A green-tinted sprite over the tile of cell (1, 0) (world [2, 4] x [0, 2], scene pixels x [12, 16), y [4, 8)): its
			// texels have no blue, every tile's has.
			RenderSnapshot snapshot = MakeTileSnapshot(setup);
			snapshot.Sprites.push_back(SpriteItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 1.0f, 0.0f)),
				.Texture = TileTextureHandle,
				.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
				.PixelsPerUnit = 4.0f,
				.Entity = UUID(91) });
			const auto blueAt = [](const Image& image, uint32_t x, uint32_t y)
			{
				// The 16 x 16 scene upscaled 4 times: scene pixel (x, y) is view pixel (4 x + 2, 4 y + 2).
				const std::span<const std::byte> row = image.GetRow(4 * y + 2);
				return std::to_integer<int>(row[4 * (4 * x + 2) + 2]);
			};
			snapshot.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .PixelsPerUnit = 2.0f, .DrawPosition = 0, .Entity = UUID(90) });
			const Image spriteOnTop = setup.Render(snapshot);
			CHECK(blueAt(spriteOnTop, 13, 5) == 0);
			snapshot.Tilemaps.front().DrawPosition = 1;
			snapshot.FrameIndex += 1;
			const Image tileOnTop = setup.Render(snapshot);
			CHECK(blueAt(tileOnTop, 13, 5) > 0);
			// Elsewhere both images show the same tiles.
			CHECK(blueAt(spriteOnTop, 2, 13) == blueAt(tileOnTop, 2, 13));
		}

		// Decision 24: the chunk bounds kept between renders follow the tilemap: a new version (another map under the same handle)
		// redraws from its own chunks, whose indices name other places, and a missing tilemap draws nothing, reported with
		// GetOrPlaceholder's code.
		TEST_CASE("Tilemaps: a tilemap's new version draws its own chunks, and a missing tilemap draws nothing" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			PublishTiles(setup, MakeTilemap(true));
			RenderSnapshot snapshot = MakeTileSnapshot(setup);
			snapshot.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .PixelsPerUnit = 2.0f, .Entity = UUID(90) });
			static_cast<void>(setup.Render(snapshot));
			CHECK(SpriteTriangles(setup) == 26);

			// Four far chunks take the indices of the chunks in view before, and the one chunk in view now takes the index of the
			// far chunk before: kept bounds would draw the four and cull the one.
			Ref<TilemapData> moved = MakeTilemap(false);
			moved->Chunks.clear();
			for (int32_t x = -40; x < -36; ++x)
				moved->Chunks.push_back(TilemapChunk{ .X = x, .Y = -40, .Tiles = { 1, 1, 1, 1 } });
			moved->Chunks.push_back(TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 1, 1, 1 } });
			setup.Assets().Publish(TilemapHandle, std::move(moved));
			snapshot.FrameIndex += 1;
			static_cast<void>(setup.Render(snapshot));
			CHECK(SpriteTriangles(setup) == 8);

			const AssetHandle missing(0x89430000ull);
			snapshot.Tilemaps.front().Tilemap = missing;
			snapshot.FrameIndex += 1;
			{
				const Test::ExpectLog log(LogLevel::Error, missing.ToString());
				static_cast<void>(setup.Render(snapshot));
			}
			CHECK(SpriteTriangles(setup) == 0);
			CHECK(std::ranges::any_of(setup.Assets().GetDiagnostics(), [missing](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == missing && diagnostic.Code == AssetMissingCode;
			}));
		}

		// Decision 24 (W21.2's review): the chunk bounds kept between renders are rebuilt when the pixels per unit or a palette
		// sheet's version changes, so a tile that grows into the view draws at once and never pops in later: chunk (2, 0),
		// outside the view, draws its frame-4 tile exactly when the tile reaches into the view. The 8 x 8 frame's tile spans
		// [1, 9] x [-3, 5] at 1 pixel per unit and [5, 9] x [-1, 3] at 2; a 4 x 4 frame's spans [5, 9] x [-1, 3] at 1.
		TEST_CASE("Tilemaps: the kept chunk bounds follow the pixels per unit and a palette sheet's new version" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			PublishTiles(setup, MakeOverhangingTilemap());
			RenderSnapshot snapshot = MakeTileSnapshot(setup);
			snapshot.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .PixelsPerUnit = 2.0f, .Entity = UUID(90) });
			const Image twoPixels = setup.Render(snapshot);
			// The 13 tiles of the cells in view; chunk (2, 0)'s tile stays outside it.
			CHECK(SpriteTriangles(setup) == 26);

			// The same tilemap at 1 pixel per unit: the tile reaches into the view, and its chunk draws it.
			snapshot.Tilemaps.front().PixelsPerUnit = OverhangPixelsPerUnit;
			snapshot.FrameIndex += 1;
			const Image onePixel = setup.Render(snapshot);
			CHECK(SpriteTriangles(setup) == 28);
			CHECK(onePixel.Pixels != twoPixels.Pixels);

			// The sheet's new version with a 4 x 4 frame 4: the tile no longer reaches the view, and its chunk is culled.
			setup.Assets().Publish(TileSheetHandle, MakeTileSheet(4));
			snapshot.FrameIndex += 1;
			const Image shrunk = setup.Render(snapshot);
			CHECK(SpriteTriangles(setup) == 26);
			CHECK(shrunk.Pixels != onePixel.Pixels);

			// Its next version, with the 8 x 8 frame again: the tile reaches into the view at once, as before.
			setup.Assets().Publish(TileSheetHandle, MakeTileSheet());
			snapshot.FrameIndex += 1;
			const Image grown = setup.Render(snapshot);
			CHECK(SpriteTriangles(setup) == 28);
			CHECK(grown.Pixels == onePixel.Pixels);
		}

		// Decision 24 (C21.2's review): the QuadBatcher loses no quad to its capacity (stream L), so a zoomed-out tilemap of
		// more than MaxQuadsPerRecord visible tiles (stream T1) never drops the sprite drawn after it: 72 chunks of 64 x 64 cells
		// of 1/128 unit, 294,912 tiles over [-4, 0.5] x [-4, 0], then a green-tinted sprite over [-3, -1]^2. Stream T1's
		// tilemaps with stream L's batches: run from W21.2's integration (decision 33).
		TEST_CASE("Tilemaps: a sprite drawn after a tilemap of more than MaxQuadsPerRecord visible tiles still draws" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			constexpr uint32_t ChunkSide = 64;
			constexpr int32_t ChunksX = 9;
			constexpr int32_t ChunksY = 8;
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->CellSize = glm::vec2(1.0f / 128.0f);
			tilemap->ChunkSize = ChunkSide;
			tilemap->Palette = { TilemapTile{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(TileSheetHandle), .Frame = 0 } };
			for (int32_t y = 0; y < ChunksY; ++y)
			{
				for (int32_t x = 0; x < ChunksX; ++x)
					tilemap->Chunks.push_back(TilemapChunk{ .X = x, .Y = y, .Tiles = std::vector<uint32_t>(ChunkSide * ChunkSide, 1) });
			}
			constexpr uint32_t TileCount = static_cast<uint32_t>(ChunksX * ChunksY) * ChunkSide * ChunkSide;
			static_assert(TileCount > QuadBatcher::MaxQuadsPerRecord);
			PublishTiles(setup, tilemap);

			RenderSnapshot snapshot = MakeTileSnapshot(setup);
			// A 4-texel frame at 512 pixels per unit is one 1/128-unit cell: the tiles lie edge to edge.
			snapshot.Tilemaps.push_back(TilemapItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-4.0f, -4.0f, 0.0f)),
				.Tilemap = TilemapHandle,
				.PixelsPerUnit = 512.0f,
				.DrawPosition = 0,
				.Entity = UUID(90) });
			snapshot.Sprites.push_back(SpriteItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(-2.0f, -2.0f, 0.0f)),
				.Texture = TileTextureHandle,
				.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f),
				.PixelsPerUnit = 4.0f,
				.Entity = UUID(91) });
			const Image image = setup.Render(snapshot);
			// Every tile and the sprite were drawn.
			CHECK(SpriteTriangles(setup) == 2 * (TileCount + 1));
			// Scene pixel (4, 12), world (-1.75, -2.25), shows the sprite: its green-tinted texel has no blue, every tile's has.
			const std::span<const std::byte> row = image.GetRow(4 * 12 + 2);
			CHECK(std::to_integer<int>(row[4 * (4 * 4 + 2) + 2]) == 0);
			// Scene pixel (8, 12), world (0.25, -2.25), shows a tile.
			CHECK(std::to_integer<int>(row[4 * (4 * 8 + 2) + 2]) > 0);
		}

		TEST_CASE("Tilemaps: a renderer drawing a tilemap recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			PublishTiles(setup, MakeTilemap(false));
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					RenderSnapshot snapshot = MakeTileSnapshot(setup);
					snapshot.Tilemaps.push_back(TilemapItem{ .Tilemap = TilemapHandle, .PixelsPerUnit = 2.0f, .Entity = UUID(90) });
					snapshot.FrameIndex = ++frame;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = setup.Renderer().Render(*list, snapshot);
					list->close();
					setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					nvrhi::ITexture* image = setup.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// As the sprites' loss case: the pixel-art scene's targets reach their steady state in the pool first.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The tiles were drawn: the bottom-left tile's texel (0, 0) is blue.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			CHECK(std::to_integer<int>(report->Before[((4 * 12 + 2) * 64 + 2) * 4 + 2]) == 255);
		}
	}

}
