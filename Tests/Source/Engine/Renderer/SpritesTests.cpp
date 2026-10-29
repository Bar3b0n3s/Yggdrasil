#include "TestsPCH.h"

#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// The Sprites stage (Docs/Decisions/0032-m21-contract.md decision 4, stream A; Roadmap M21 "An integer-scale sprite reads
// back its source texels under both API caps"; plan §7 item 3: every new GPU owner has a loss test through the shared helper).
// The gpu suite runs every case under API 1.4 and again capped at 1.3. Skipped until stream A draws the sprites.

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
		TEST_CASE("Sprites: an integer-scale sprite reads back its source texels" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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

		TEST_CASE("Sprites: the Sprites stage draws the snapshot's sprites in their order" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
		}

		TEST_CASE("Sprites: a renderer drawing sprites recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(SpriteTextureHandle, MakeSpriteTexture());
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
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
				ENGINE_TRY_ASSIGN(const Image pixels, readback.ReadTexture(*image));
				return pixels.Pixels;
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The sprite was drawn: texel (0, 0) at view pixel (16, 16), not the clear colour.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			CHECK(std::to_integer<int>(report->Before[(16 * 64 + 16) * 4 + 2]) == 255);
		}
	}

}
