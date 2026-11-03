#include "TestsPCH.h"

#include "Engine/Asset/AssetDiagnostic.h"
#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// The Sprites stage (Docs/Decisions/0032-m21-contract.md decision 4, stream A; Roadmap M21 "An integer-scale sprite reads
// back its source texels under both API caps"; plan §7 item 3: every new GPU owner has a loss test through the shared helper).
// The gpu suite runs every case under API 1.4 and again capped at 1.3.

namespace Engine {

	namespace {

		// 8 x 8 texels of distinct sRGB values: texel (x, y) is (32 x, 32 y, 255 - 16 x, 255).
		constexpr uint32_t SpriteTexels = 8;
		const AssetHandle SpriteTextureHandle = UUID(8920);
		// 8 x 8 opaque white texels, which a sprite's Color tints. The test's own texture, so these cases need neither stream
		// B's built-in sprite textures nor its catalogue.
		const AssetHandle WhiteSpriteTextureHandle = UUID(8921);

		Ref<TextureData> MakeEmptySpriteTexture()
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = SpriteTexels;
			texture->Height = SpriteTexels;
			texture->Mips = { TextureMip{ .Width = SpriteTexels, .Height = SpriteTexels, .Offset = 0, .Size = SpriteTexels * SpriteTexels * 4 } };
			texture->Pixels.reserve(static_cast<size_t>(SpriteTexels) * SpriteTexels * 4);
			return texture;
		}

		Ref<TextureData> MakeSpriteTexture()
		{
			Ref<TextureData> texture = MakeEmptySpriteTexture();
			for (uint32_t y = 0; y < SpriteTexels; ++y)
			{
				for (uint32_t x = 0; x < SpriteTexels; ++x)
				{
					texture->Pixels.push_back(static_cast<std::byte>(32 * x));
					texture->Pixels.push_back(static_cast<std::byte>(32 * y));
					texture->Pixels.push_back(static_cast<std::byte>(255 - 16 * x));
					texture->Pixels.push_back(std::byte{ 255 });
				}
			}
			return texture;
		}

		Ref<TextureData> MakeWhiteSpriteTexture()
		{
			Ref<TextureData> texture = MakeEmptySpriteTexture();
			texture->Pixels.assign(static_cast<size_t>(SpriteTexels) * SpriteTexels * 4, std::byte{ 255 });
			return texture;
		}

		// A 64 x 64 view of a 16 x 16 pixel-art scene (scale 4): an orthographic camera at z = 10 whose view is 8 units high, so
		// a unit is 2 scene pixels and a sprite of 2 pixels per unit shows each texel as one scene pixel.
		RenderSnapshot MakeSpriteSnapshot(const Test::SceneRendererIntegrationFixture& setup)
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
			snapshot.Sprites.push_back(SpriteItem{ .Texture = SpriteTextureHandle, .PixelsPerUnit = 2.0f, .Entity = UUID(77) });
			return snapshot;
		}

		glm::u8vec4 SpritePixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> row = image.GetRow(y);
			const size_t offset = 4 * static_cast<size_t>(x);
			return glm::u8vec4(std::to_integer<uint8_t>(row[offset]), std::to_integer<uint8_t>(row[offset + 1]), std::to_integer<uint8_t>(row[offset + 2]),
				std::to_integer<uint8_t>(row[offset + 3]));
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Sprites: an integer-scale sprite reads back its source texels" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(SpriteTextureHandle, MakeSpriteTexture());
			const Image image = setup.Render(MakeSpriteSnapshot(setup));
			// The sprite covers scene pixels [4, 12) in both axes: view pixels [16, 48), each texel a 4 x 4 block.
			for (uint32_t y = 16; y < 48; ++y)
			{
				for (uint32_t x = 16; x < 48; ++x)
				{
					const uint32_t texelX = (x - 16) / 4;
					const uint32_t texelY = (y - 16) / 4;
					CHECK(SpritePixel(image, x, y) == glm::u8vec4(32 * texelX, 32 * texelY, 255 - 16 * texelX, 255));
				}
			}
		}

		TEST_CASE("Sprites: the Sprites stage draws the snapshot's sprites in their order" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(WhiteSpriteTextureHandle, MakeWhiteSpriteTexture());
			RenderSnapshot snapshot = MakeSpriteSnapshot(setup);
			snapshot.Sprites.clear();
			// Two overlapping tinted white squares of 8 x 8 texels (scene pixels [4, 12), the green one shifted 2 pixels right):
			// the later one in the snapshot (its draw order) covers the overlap, which holds scene pixel (8, 8).
			const SpriteItem red{ .Texture = WhiteSpriteTextureHandle, .Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), .PixelsPerUnit = 2.0f, .Entity = UUID(1) };
			SpriteItem green = red;
			green.Color = glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			green.World = glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			green.Entity = UUID(2);
			snapshot.Sprites = { red, green };
			const Image greenOnTop = setup.Render(snapshot);
			CHECK(SpritePixel(greenOnTop, 32, 32) == glm::u8vec4(0, 255, 0, 255));
			snapshot.Sprites = { green, red };
			snapshot.FrameIndex += 1;
			const Image redOnTop = setup.Render(snapshot);
			CHECK(SpritePixel(redOnTop, 32, 32) == glm::u8vec4(255, 0, 0, 255));

			// The Sprites row counts one quad (two triangles) a sprite in one draw, the sprites sharing a texture; a sprite
			// wholly outside the view is culled, whichever side it leaves by.
			const RenderStats drawnStats = setup.Renderer().GetRenderStats();
			const RenderPassStats* both = Test::SceneRendererIntegrationFixture::FindPass(drawnStats, "Sprites");
			REQUIRE(both != nullptr);
			CHECK(both->Triangles == 4);
			CHECK(both->DrawCalls == 1);
			for (const glm::vec3& outside : { glm::vec3(10.0f, 0.0f, 0.0f), glm::vec3(0.0f, -10.0f, 0.0f), glm::vec3(0.0f, 0.0f, 30.0f) })
			{
				CAPTURE(outside);
				SpriteItem culled = red;
				culled.World = glm::translate(glm::mat4(1.0f), outside);
				snapshot.Sprites = { green, red, culled };
				snapshot.FrameIndex += 1;
				const Image unchanged = setup.Render(snapshot);
				CHECK(unchanged.Pixels == redOnTop.Pixels);
				const RenderStats culledStats = setup.Renderer().GetRenderStats();
				const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(culledStats, "Sprites");
				REQUIRE(pass != nullptr);
				CHECK(pass->Triangles == 4);
			}
		}

		// Decision 4 (W21.1's review): sprites draw in the Lit view only, so the data views, the material views, the Overdraw view
		// and the Lit view's wireframe show none, and the Sprites row counts no quad.
		TEST_CASE("Sprites: data views, material views and the wireframe draw no sprites" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(SpriteTextureHandle, MakeSpriteTexture());
			RenderSnapshot snapshot = MakeSpriteSnapshot(setup);
			// The sprite's texel (4, 4) at scene pixel (8, 8), view pixel (32, 32), in the Lit view.
			const glm::u8vec4 texel(128, 128, 191, 255);
			const auto spriteQuads = [&setup]()
			{
				const RenderStats stats = setup.Renderer().GetRenderStats();
				const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, "Sprites");
				return pass != nullptr ? pass->Triangles : 0u;
			};
			REQUIRE(SpritePixel(setup.Render(snapshot), 32, 32) == texel);
			REQUIRE(spriteQuads() == 2);
			for (uint32_t view = 1; view < RenderDebugViewCount; ++view)
			{
				const auto debugView = static_cast<RenderDebugView>(view);
				CAPTURE(RenderDebugViewToString(debugView));
				snapshot.DebugView = debugView;
				snapshot.FrameIndex += 1;
				const Image image = setup.Render(snapshot);
				CHECK(SpritePixel(image, 32, 32) != texel);
				CHECK(spriteQuads() == 0);
			}
			snapshot.DebugView = RenderDebugView::Lit;
			snapshot.Flags = snapshot.Flags | RenderViewFlags::Wireframe;
			snapshot.FrameIndex += 1;
			const Image wireframe = setup.Render(snapshot);
			CHECK(SpritePixel(wireframe, 32, 32) != texel);
			CHECK(spriteQuads() == 0);
		}

		// SpriteRendererComponent.h's one rule: a missing texture draws its placeholder, the 64 x 64 Missing checker, whole over
		// the sprite's quad, while a missing sheet (reported with GetOrPlaceholder's code) or a frame the sheet lacks draws
		// nothing.
		TEST_CASE("Sprites: a missing texture draws its placeholder whole, and a missing sheet or an out-of-range frame draws nothing" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			const AssetHandle missingTexture(0x89500000ull);
			const AssetHandle missingSheet(0x89510000ull);
			const AssetHandle sheetHandle = UUID(8952);
			Ref<SpriteSheetData> sheet = CreateRef<SpriteSheetData>();
			sheet->Frames.push_back(SpriteFrame{ .Rect = { .Width = 8, .Height = 8 } });
			setup.Assets().Publish(sheetHandle, sheet);

			RenderSnapshot snapshot = MakeSpriteSnapshot(setup);
			// The placeholder's 64 texels at 16 pixels per unit: 4 units, scene pixels [4, 12) in both axes, 8 texels a pixel. The
			// two sprites that draw nothing would cover the top-left scene pixels [0, 4) in white.
			const glm::mat4 topLeft = glm::translate(glm::mat4(1.0f), glm::vec3(-3.0f, 3.0f, 0.0f));
			snapshot.Sprites = {
				SpriteItem{ .Texture = missingTexture, .PixelsPerUnit = 16.0f, .Entity = UUID(1) },
				SpriteItem{ .World = topLeft, .Sheet = missingSheet, .PixelsPerUnit = 4.0f, .Entity = UUID(2) },
				SpriteItem{ .World = topLeft, .Sheet = sheetHandle, .Frame = 1, .PixelsPerUnit = 4.0f, .Entity = UUID(3) },
			};
			Image image;
			{
				const Test::ExpectLog texture(LogLevel::Error, missingTexture.ToString());
				const Test::ExpectLog sheetMissing(LogLevel::Error, missingSheet.ToString());
				image = setup.Render(snapshot);
			}
			CHECK(std::ranges::any_of(setup.Assets().GetDiagnostics(), [missingSheet](const AssetDiagnostic& diagnostic)
			{
				return diagnostic.Asset == missingSheet && diagnostic.Code == AssetMissingCode;
			}));
			// Neighbouring scene pixels show neighbouring checker squares: one magenta, one black.
			const glm::u8vec4 magenta(255, 0, 255, 255);
			const glm::u8vec4 black(0, 0, 0, 255);
			const glm::u8vec4 first = SpritePixel(image, 16, 16);
			const glm::u8vec4 second = SpritePixel(image, 20, 16);
			CHECK((first == magenta || first == black));
			CHECK((second == magenta || second == black));
			CHECK(first != second);
			// The top-left scene pixels keep the clear colour.
			for (uint32_t y = 0; y < 16; ++y)
			{
				for (uint32_t x = 0; x < 16; ++x)
					CHECK(SpritePixel(image, x, y) == black);
			}
		}

		// Snapshot data the schemas never produce is a partial error (Architecture §8.2): the sprite is skipped, the others drawn.
		TEST_CASE("Sprites: a sprite with a non-finite matrix or pixels per unit is skipped with an InvalidArgument error" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(SpriteTextureHandle, MakeSpriteTexture());
			GraphicsDevice& device = gpu.GetDevice();
			const RenderSnapshot drawn = MakeSpriteSnapshot(setup);
			const Image expected = setup.Render(drawn);
			for (const bool matrix : { true, false })
			{
				CAPTURE(matrix);
				RenderSnapshot snapshot = drawn;
				snapshot.FrameIndex += matrix ? 1 : 2;
				SpriteItem invalid = snapshot.Sprites.front();
				invalid.Entity = UUID(78);
				if (matrix)
					invalid.World[3][0] = std::numeric_limits<float>::infinity();
				else
					invalid.PixelsPerUnit = 0.0f;
				snapshot.Sprites.push_back(invalid);
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				const Status rendered = setup.Renderer().Render(**list, snapshot);
				(*list)->close();
				setup.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(setup.Read().Pixels == expected.Pixels);
			}
		}

		TEST_CASE("Sprites: a renderer drawing sprites recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(SpriteTextureHandle, MakeSpriteTexture());
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			// One use renders the snapshot into each of the renderer's frames in flight, so its profiler holds the timer queries
			// of every frame slot (each slot's are created at its first frame) before the loss as after the recovery.
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					RenderSnapshot snapshot = MakeSpriteSnapshot(setup);
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
			// The fixture creates its renderer's targets at the 64 x 64 view size, which the pixel-art snapshot's 16 x 16 scene
			// leaves free in the pool until EndFrame releases them, DefaultKeepFrames frames later, while the recovery creates
			// the targets of the last scene resolution only (SceneRendererRecovery.cpp): the renderer reaches that steady state
			// first, so the live counts before the loss are the ones the recovery reproduces.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The sprite was drawn: texel (0, 0) at view pixel (16, 16), not the clear colour.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			CHECK(std::to_integer<int>(report->Before[(16 * 64 + 16) * 4 + 2]) == 255);
		}
	}

}
