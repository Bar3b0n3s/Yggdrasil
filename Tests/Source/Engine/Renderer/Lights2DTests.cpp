#include "TestsPCH.h"

#include "Engine/Asset/TextureCompression.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Light2DShading.h"
#include "Engine/Renderer/Lights2DPass.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// The Lights2D pass and lit sprites (M21, C21.2, R-2D-LIGHT; Docs/Decisions/0032-m21-contract.md decisions 21 and 22, stream
// L; Roadmap M21 "A lit sprite matches its CPU reference", plan §3.5 "unlit sprites and existing goldens are unchanged"; plan
// §7 item 3: every new GPU owner has a loss test through the shared helper). The gpu suite runs every case under API 1.4 and
// again capped at 1.3.

namespace Engine {

	namespace {

		// 8 x 8 texels of distinct sRGB values: texel (x, y) is (32 x, 32 y, 255 - 16 x, 255).
		constexpr uint32_t LitTexels = 8;
		const AssetHandle LitTextureHandle = UUID(8930);
		const AssetHandle NormalMapHandle = UUID(8931);
		// Every texel of the normal map encodes the normal (-1, 0, 1) / sqrt(2): it leans towards -X, the image's left.
		constexpr uint8_t LeaningNormalRed = 37;    // round((1 - 1 / sqrt(2)) / 2 x 255)
		constexpr uint8_t LeaningNormalGreen = 128; // round(1 / 2 x 255) is 128: n.y = 0 to within 1/255
		constexpr uint8_t LeaningNormalBlue = 218;  // round((1 + 1 / sqrt(2)) / 2 x 255)

		Ref<TextureData> MakeLitTexture(TextureFormat format)
		{
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = format;
			texture->Width = LitTexels;
			texture->Height = LitTexels;
			texture->Mips = { TextureMip{ .Width = LitTexels, .Height = LitTexels, .Offset = 0, .Size = LitTexels * LitTexels * 4 } };
			texture->Pixels.reserve(static_cast<size_t>(LitTexels) * LitTexels * 4);
			return texture;
		}

		Ref<TextureData> MakeColorTexture()
		{
			Ref<TextureData> texture = MakeLitTexture(TextureFormat::RGBA8Srgb);
			for (uint32_t y = 0; y < LitTexels; ++y)
			{
				for (uint32_t x = 0; x < LitTexels; ++x)
				{
					texture->Pixels.push_back(static_cast<std::byte>(32 * x));
					texture->Pixels.push_back(static_cast<std::byte>(32 * y));
					texture->Pixels.push_back(static_cast<std::byte>(255 - 16 * x));
					texture->Pixels.push_back(std::byte{ 255 });
				}
			}
			return texture;
		}

		Ref<TextureData> MakeNormalMap()
		{
			Ref<TextureData> texture = MakeLitTexture(TextureFormat::RGBA8Unorm);
			for (uint32_t texel = 0; texel < LitTexels * LitTexels; ++texel)
			{
				texture->Pixels.push_back(std::byte{ LeaningNormalRed });
				texture->Pixels.push_back(std::byte{ LeaningNormalGreen });
				texture->Pixels.push_back(std::byte{ LeaningNormalBlue });
				texture->Pixels.push_back(std::byte{ 255 });
			}
			return texture;
		}

		// MakeNormalMap's texels as TextureImporter's Auto compression stores a normal map (Docs/Decisions/0034-m23-contract.md
		// decision 13): BC5, x and y in R and G, no z.
		Ref<TextureData> MakeBc5NormalMap()
		{
			const Ref<TextureData> rgba = MakeNormalMap();
			Result<Buffer> blocks = CompressTextureBlocks(TextureBlockCodec::BC5, LitTexels, LitTexels, rgba->Pixels);
			REQUIRE_MESSAGE(blocks.has_value(), blocks.error().ToString());
			Ref<TextureData> texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::BC5Unorm;
			texture->Width = LitTexels;
			texture->Height = LitTexels;
			texture->Mips = { TextureMip{ .Width = LitTexels, .Height = LitTexels, .Offset = 0, .Size = blocks->size() } };
			texture->Pixels = std::move(*blocks);
			REQUIRE_MESSAGE(ValidateTextureData(*texture).has_value(), ValidateTextureData(*texture).error().ToString());
			return texture;
		}

		// A 64 x 64 view of a 16 x 16 pixel-art scene (scale 4): an orthographic camera at z = 10 whose view is 8 units high, so a
		// unit is 2 scene pixels and a sprite of 2 pixels per unit shows each texel as one scene pixel, scene pixels [4, 12).
		RenderSnapshot MakeLitSnapshot(const Test::SceneRendererIntegrationFixture& setup)
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
			snapshot.Sprites.push_back(SpriteItem{ .Texture = LitTextureHandle, .PixelsPerUnit = 2.0f, .Lit = true, .Entity = UUID(77) });
			// A warm point light to the upper left of the sprite's centre, fading out across it, and a dim global light. Both reach
			// the view, so the snapshot's Lights2D are the lights the view draws, EvaluateLights2D's input (Light2DShading.h).
			snapshot.Lights2D.push_back(Light2DItem{ .Type = RenderLight2DType::Point, .Position = glm::vec3(-1.0f, 1.0f, 0.0f), .Color = glm::vec3(1.0f, 0.8f, 0.6f), .Intensity = 1.5f, .Radius = 3.0f, .Falloff = 0.5f, .Height = 1.0f, .Entity = UUID(78) });
			snapshot.Lights2D.push_back(Light2DItem{ .Type = RenderLight2DType::Global, .Color = glm::vec3(0.1f), .Intensity = 1.0f, .Entity = UUID(79) });
			return snapshot;
		}

		glm::u8vec4 LitPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> row = image.GetRow(y);
			const size_t offset = 4 * static_cast<size_t>(x);
			return glm::u8vec4(std::to_integer<uint8_t>(row[offset]), std::to_integer<uint8_t>(row[offset + 1]), std::to_integer<uint8_t>(row[offset + 2]),
				std::to_integer<uint8_t>(row[offset + 3]));
		}

		// The world point scene pixel (x, y) of the 16 x 16 view shows: 2 pixels a unit, the origin at the scene's centre.
		glm::vec2 ScenePixelToWorld(uint32_t x, uint32_t y)
		{
			return glm::vec2((static_cast<float>(x) + 0.5f - 8.0f) / 2.0f, (8.0f - (static_cast<float>(y) + 0.5f)) / 2.0f);
		}

		// The 8-bit value the 2D colour path shows for linear `value` (clamped, sRGB-encoded).
		uint8_t EncodeLitChannel(double value)
		{
			return static_cast<uint8_t>(std::lround(Test::LinearToSrgb(std::clamp(value, 0.0, 1.0)) * 255.0));
		}

		// Every scene pixel of the sprite, read at the centre of its 4 x 4 block of view pixels, matches texel x light within
		// `tolerance` 8-bit levels (the half-float accumulation's rounding), its alpha opaque; with `flipX` scene pixel (x, y)
		// shows texel (11 - x, y - 4).
		void CheckLitSprite(const Image& image, const RenderSnapshot& snapshot, const std::optional<glm::vec3>& normal, int tolerance, bool flipX = false)
		{
			for (uint32_t y = 4; y < 12; ++y)
			{
				for (uint32_t x = 4; x < 12; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					const uint32_t texelX = flipX ? 11 - x : x - 4;
					const uint32_t texelY = y - 4;
					const glm::dvec3 texel(Test::SrgbToLinear(32.0 * texelX / 255.0), Test::SrgbToLinear(32.0 * texelY / 255.0),
						Test::SrgbToLinear((255.0 - 16.0 * texelX) / 255.0));
					const glm::dvec3 light = glm::dvec3(EvaluateLights2D(snapshot.Lights2D, ScenePixelToWorld(x, y), normal));
					const glm::dvec3 expected = texel * light;
					const glm::u8vec4 actual = LitPixel(image, 4 * x + 2, 4 * y + 2);
					CHECK(std::abs(static_cast<int>(actual.r) - EncodeLitChannel(expected.x)) <= tolerance);
					CHECK(std::abs(static_cast<int>(actual.g) - EncodeLitChannel(expected.y)) <= tolerance);
					CHECK(std::abs(static_cast<int>(actual.b) - EncodeLitChannel(expected.z)) <= tolerance);
					CHECK(actual.a == 255);
				}
			}
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Lights2D: a lit sprite matches its CPU reference" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			const RenderSnapshot snapshot = MakeLitSnapshot(setup);
			const Image image = setup.Render(snapshot);
			// The point light reaches part of the sprite and fades across it; the global light lifts the rest.
			CHECK(EvaluateLights2D(snapshot.Lights2D, ScenePixelToWorld(5, 5), std::nullopt).x > EvaluateLights2D(snapshot.Lights2D, ScenePixelToWorld(11, 11), std::nullopt).x);
			CheckLitSprite(image, snapshot, std::nullopt, 2);
			// The Lights2D row counts the lights' draws.
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* lights = Test::SceneRendererIntegrationFixture::FindPass(stats, "Lights2D");
			REQUIRE(lights != nullptr);
			CHECK(lights->DrawCalls >= 2);
		}

		// Decision 22 (C21.2's review): the cap counts only the lights that reach the view. 100 point lights far off the view
		// come first in the snapshot's order, then MaxVisibleLights2D + 1 dim global lights: the view draws the first
		// MaxVisibleLights2D globals, and the CPU reference over the lights that reach the view matches its image.
		TEST_CASE("Lights2D: a view draws the first MaxVisibleLights2D lights that reach it, and lights off the view do not count"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			RenderSnapshot snapshot = MakeLitSnapshot(setup);
			snapshot.Lights2D.clear();
			for (uint32_t index = 0; index < 100; ++index)
			{
				snapshot.Lights2D.push_back(Light2DItem{ .Type = RenderLight2DType::Point,
					.Position = glm::vec3(500.0f, 500.0f, 0.0f),
					.Intensity = 1000.0f,
					.Radius = 1.0f,
					.Entity = UUID(1000 + index) });
			}
			// The lights that reach the view: each global adds 2^-11, exact in half floats, so the first MaxVisibleLights2D add
			// up to 0.5 and the last one is skipped.
			RenderSnapshot reachable = MakeLitSnapshot(setup);
			reachable.Lights2D.clear();
			for (uint32_t index = 0; index <= MaxVisibleLights2D; ++index)
			{
				const Light2DItem global{ .Type = RenderLight2DType::Global, .Intensity = 1.0f / 2048.0f, .Entity = UUID(3000 + index) };
				snapshot.Lights2D.push_back(global);
				reachable.Lights2D.push_back(global);
			}
			const glm::vec2 center = ScenePixelToWorld(8, 8);
			CHECK(EvaluateLights2D(reachable.Lights2D, center, std::nullopt).x == doctest::Approx(0.5));
			// A cap over the snapshot's own order, off-view lights included, would leave out 100 of the globals.
			CHECK(EvaluateLights2D(snapshot.Lights2D, center, std::nullopt).x < EvaluateLights2D(reachable.Lights2D, center, std::nullopt).x);
			// The renderer skips the last global, logging the first skip once.
			Test::ExpectLog skipped(LogLevel::Warn, "2D light");
			CheckLitSprite(setup.Render(snapshot), reachable, std::nullopt, 2);
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* lights = Test::SceneRendererIntegrationFixture::FindPass(stats, "Lights2D");
			REQUIRE(lights != nullptr);
			CHECK(lights->DrawCalls == MaxVisibleLights2D);
			snapshot.FrameIndex += 1;
			CheckLitSprite(setup.Render(snapshot), reachable, std::nullopt, 2);
			CHECK(skipped.GetMatchCount() == 1);
		}

		TEST_CASE("Lights2D: a normal-mapped lit sprite matches its CPU reference" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			setup.Assets().Publish(NormalMapHandle, MakeNormalMap());
			RenderSnapshot snapshot = MakeLitSnapshot(setup);
			snapshot.Sprites.front().NormalMap = NormalMapHandle;
			// The unrotated, unflipped sprite's tangent frame is the world's: its normal is the map's.
			const glm::vec3 normal = glm::normalize(glm::vec3(2.0f * LeaningNormalRed / 255.0f - 1.0f, 2.0f * LeaningNormalGreen / 255.0f - 1.0f,
				2.0f * LeaningNormalBlue / 255.0f - 1.0f));
			CheckLitSprite(setup.Render(snapshot), snapshot, normal, 2);
			// FlipX mirrors the tangent frame: the normal then leans towards +X. Right of the light (at x = -1), where the
			// unflipped normal leaned towards it, the mirrored one leans away from it.
			snapshot.Sprites.front().FlipX = true;
			snapshot.FrameIndex += 1;
			const Image flipped = setup.Render(snapshot);
			const glm::vec3 mirrored(-normal.x, normal.y, normal.z);
			const glm::vec2 rightOfLight = ScenePixelToWorld(7, 4);
			CHECK(EvaluateLights2D(snapshot.Lights2D, rightOfLight, mirrored).x < EvaluateLights2D(snapshot.Lights2D, rightOfLight, normal).x);
			// Scene pixel (4, 4) now shows texel (7, 0).
			const glm::vec2 world = ScenePixelToWorld(4, 4);
			const double texelRed = Test::SrgbToLinear(32.0 * 7 / 255.0);
			const int expectedRed = EncodeLitChannel(texelRed * EvaluateLights2D(snapshot.Lights2D, world, mirrored).x);
			CHECK(std::abs(static_cast<int>(LitPixel(flipped, 18, 18).r) - expectedRed) <= 2);
			// Every pixel shows its mirrored texel lit with the mirrored normal.
			CheckLitSprite(flipped, snapshot, mirrored, 2, true);
		}

		// M23's milestone review (Docs/Decisions/0034-m23-contract.md, "Milestone review" item 1): a normal map imported with
		// Compression Auto, the default, is BC5, which samples as (x, y, 0, 1) on a device with BC and decodes to (x, y, 0, 255)
		// through the decode fallback, so the Normals target reconstructs z from x and y, as the 3D path does: the sprite is
		// lit as with the same normal map imported with None (RGBA8), within the case above's tolerance, on both devices. Read
		// as 2 rgb - 1, the missing z would turn every normal away from the viewer and the lights.
		TEST_CASE("Lights2D: a lit sprite's BC5 normal map lights it as its uncompressed import does, with BC and through the decode fallback"
			* doctest::test_suite(Test::GpuSuite))
		{
			const Ref<TextureData> compressed = MakeBc5NormalMap();
			const glm::vec3 normal = glm::normalize(glm::vec3(2.0f * LeaningNormalRed / 255.0f - 1.0f, 2.0f * LeaningNormalGreen / 255.0f - 1.0f,
				2.0f * LeaningNormalBlue / 255.0f - 1.0f));
			for (const bool withBc : { true, false })
			{
				CAPTURE(withBc);
				Test::HeadlessGpuFixture gpu({ .DisableTextureCompressionBC = !withBc });
				ENGINE_REQUIRE_GPU(gpu);
				REQUIRE(gpu.GetDevice().GetInfo().TextureCompressionBC == withBc);
				std::optional<Image> uncompressed;
				std::optional<Image> blockCompressed;
				RenderSnapshot snapshot;
				{
					Test::SceneRendererIntegrationFixture setup(gpu);
					setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
					setup.Assets().Publish(NormalMapHandle, MakeNormalMap());
					snapshot = MakeLitSnapshot(setup);
					snapshot.Sprites.front().NormalMap = NormalMapHandle;
					uncompressed = setup.Render(snapshot);
					setup.Assets().Publish(NormalMapHandle, compressed);
					snapshot.FrameIndex += 1;
					blockCompressed = setup.Render(snapshot);
				}
				CheckLitSprite(*uncompressed, snapshot, normal, 2);
				CheckLitSprite(*blockCompressed, snapshot, normal, 2);
				for (uint32_t y = 4; y < 12; ++y)
				{
					for (uint32_t x = 4; x < 12; ++x)
					{
						CAPTURE(x);
						CAPTURE(y);
						const glm::u8vec4 expected = LitPixel(*uncompressed, 4 * x + 2, 4 * y + 2);
						const glm::u8vec4 actual = LitPixel(*blockCompressed, 4 * x + 2, 4 * y + 2);
						CHECK(std::abs(static_cast<int>(actual.r) - static_cast<int>(expected.r)) <= 2);
						CHECK(std::abs(static_cast<int>(actual.g) - static_cast<int>(expected.g)) <= 2);
						CHECK(std::abs(static_cast<int>(actual.b) - static_cast<int>(expected.b)) <= 2);
					}
				}
				gpu.GetDevice().RunGarbageCollection();
			}
		}

		// Decision 22's one normal per pixel: a flat lit sprite drawn over a normal-mapped one gives the pixels it covers the
		// flat normal (each light whole), and the normal-mapped sprite's other pixels keep its normals; the Normals record
		// splits its draws where the normal map changes.
		TEST_CASE("Lights2D: a pixel takes the light with the normal of its topmost lit sprite" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			setup.Assets().Publish(NormalMapHandle, MakeNormalMap());
			RenderSnapshot snapshot = MakeLitSnapshot(setup);
			snapshot.Sprites.front().NormalMap = NormalMapHandle;
			// A flat lit sprite 2 units (4 scene pixels) to the right, drawn after: scene pixels [8, 16) x [4, 12).
			SpriteItem flat = snapshot.Sprites.front();
			flat.World = glm::translate(glm::mat4(1.0f), glm::vec3(2.0f, 0.0f, 0.0f));
			flat.NormalMap = AssetHandle();
			flat.Entity = UUID(80);
			snapshot.Sprites.push_back(flat);
			const Image image = setup.Render(snapshot);
			const glm::vec3 normal = glm::normalize(glm::vec3(2.0f * LeaningNormalRed / 255.0f - 1.0f, 2.0f * LeaningNormalGreen / 255.0f - 1.0f,
				2.0f * LeaningNormalBlue / 255.0f - 1.0f));
			for (uint32_t y = 4; y < 12; ++y)
			{
				for (uint32_t x = 4; x < 16; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					const bool top = x >= 8;
					const uint32_t texelX = top ? x - 8 : x - 4;
					const uint32_t texelY = y - 4;
					const glm::dvec3 texel(Test::SrgbToLinear(32.0 * texelX / 255.0), Test::SrgbToLinear(32.0 * texelY / 255.0),
						Test::SrgbToLinear((255.0 - 16.0 * texelX) / 255.0));
					const std::optional<glm::vec3> pixelNormal = top ? std::nullopt : std::optional<glm::vec3>(normal);
					const glm::dvec3 expected = texel * glm::dvec3(EvaluateLights2D(snapshot.Lights2D, ScenePixelToWorld(x, y), pixelNormal));
					const glm::u8vec4 actual = LitPixel(image, 4 * x + 2, 4 * y + 2);
					CHECK(std::abs(static_cast<int>(actual.r) - EncodeLitChannel(expected.x)) <= 2);
					CHECK(std::abs(static_cast<int>(actual.g) - EncodeLitChannel(expected.y)) <= 2);
					CHECK(std::abs(static_cast<int>(actual.b) - EncodeLitChannel(expected.z)) <= 2);
				}
			}
			// Where the flat sprite covers the normal-mapped one, the pixel takes the point light whole, more than the leaning
			// normal beneath would take: scene pixel (9, 6) is lit with the flat normal.
			const glm::vec2 covered = ScenePixelToWorld(9, 6);
			CHECK(EvaluateLights2D(snapshot.Lights2D, covered, std::nullopt).x > EvaluateLights2D(snapshot.Lights2D, covered, normal).x);
			// The Lights2D row: the two lit quads' normals (one draw per normal map) and the two lights.
			const RenderStats stats = setup.Renderer().GetRenderStats();
			const RenderPassStats* lights = Test::SceneRendererIntegrationFixture::FindPass(stats, "Lights2D");
			REQUIRE(lights != nullptr);
			CHECK(lights->DrawCalls == 4);
		}

		TEST_CASE("Lights2D: unlit sprites keep their texels, and a view without lit items records no Lights2D pass" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			RenderSnapshot snapshot = MakeLitSnapshot(setup);
			// Unlit, the sprite reads back its source texels exactly, however bright the lights, and nothing records Lights2D.
			snapshot.Sprites.front().Lit = false;
			const Image unlit = setup.Render(snapshot);
			for (uint32_t y = 4; y < 12; ++y)
			{
				for (uint32_t x = 4; x < 12; ++x)
					CHECK(LitPixel(unlit, 4 * x + 2, 4 * y + 2) == glm::u8vec4(32 * (x - 4), 32 * (y - 4), 255 - 16 * (x - 4), 255));
			}
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(setup.Renderer().GetRenderStats(), "Lights2D") == nullptr);
			// The same image without any light: lights change nothing an unlit sprite shows.
			snapshot.Lights2D.clear();
			snapshot.FrameIndex += 1;
			CHECK(setup.Render(snapshot).Pixels == unlit.Pixels);
			// A lit sprite without any light is black.
			snapshot.Sprites.front().Lit = true;
			snapshot.FrameIndex += 1;
			CHECK(LitPixel(setup.Render(snapshot), 32, 32) == glm::u8vec4(0, 0, 0, 255));
		}

		TEST_CASE("Lights2D: a renderer drawing 2D lights recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu);
			setup.Assets().Publish(LitTextureHandle, MakeColorTexture());
			setup.Assets().Publish(NormalMapHandle, MakeNormalMap());
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 50;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			// One use renders the snapshot, a normal-mapped lit sprite, into each of the renderer's frames in flight (the light
			// targets come from its pool, the Lights2D pipeline from the pipeline set).
			test.Use = [&device, &setup, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					RenderSnapshot snapshot = MakeLitSnapshot(setup);
					snapshot.Sprites.front().NormalMap = NormalMapHandle;
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
			// The lit sprite was drawn: its centre is neither the clear colour nor black.
			REQUIRE(report->Before.size() == 64 * 64 * 4);
			CHECK(std::to_integer<int>(report->Before[(32 * 64 + 32) * 4 + 2]) > 0);
		}

		TEST_CASE("Lights2DPass: a record whose binding set fails returns the error and records nothing" * doctest::test_suite(Test::GpuSuite))
		{
			// NVRHI's validation refuses a texture handle whose description has no mip level (as the QuadBatcher's case): the
			// pass creates its one binding set before its first command, so the failed record clears and draws nothing, and
			// the same record with a texture it can bind then draws its light.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<Lights2DPass>> pass = Lights2DPass::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			CHECK((*pass)->GetPipelineCount() == Lights2DPass::PipelineCount);
			nvrhi::TextureDesc desc;
			desc.width = 4;
			desc.height = 4;
			desc.format = Lights2DLightFormat;
			desc.isRenderTarget = true;
			desc.isShaderResource = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = "Lights2DTests.Light";
			Result<nvrhi::TextureHandle> light = device.CreateTexture(desc);
			desc.format = Lights2DNormalsFormat;
			desc.debugName = "Lights2DTests.Normals";
			Result<nvrhi::TextureHandle> normals = device.CreateTexture(desc);
			REQUIRE(light.has_value());
			REQUIRE(normals.has_value());
			Result<nvrhi::FramebufferHandle> framebuffer = device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*light));
			REQUIRE(framebuffer.has_value());
			nvrhi::TextureDesc unbindableDesc = (*normals)->getDesc();
			unbindableDesc.mipLevels = 0;
			unbindableDesc.debugName = "Lights2DTests.Unbindable";
			nvrhi::TextureHandle unbindable = device.GetNvrhiDevice()->createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image,
				(*normals)->getNativeObject(nvrhi::ObjectTypes::VK_Image), unbindableDesc);
			REQUIRE(unbindable != nullptr);
			const std::array<Light2DItem, 1> lights = { Light2DItem{ .Type = RenderLight2DType::Global } };
			PassBindingCache bindings;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			RenderPassCounters counters;
			{
				Test::ExpectLog refused(LogLevel::Error, "does not intersect");
				(*list)->open();
				const Status recorded = (*pass)->Record(**list, bindings, { .Lights = lights, .Normals = unbindable.Get(), .Light = *framebuffer }, counters);
				(*list)->close();
				device.ExecuteCommandList(**list);
				REQUIRE_FALSE(recorded.has_value());
				CHECK(recorded.error().GetCode() == ErrorCode::Gpu);
			}
			CHECK(counters.DrawCalls == 0);
			CHECK(bindings.GetSize() == 0);
			device.GetDiagnostics().ResetCounts();
			unbindable = nullptr;

			(*list)->open();
			const Status drawn = (*pass)->Record(**list, bindings, { .Lights = lights, .Normals = *normals, .Light = *framebuffer }, counters);
			(*list)->close();
			device.ExecuteCommandList(**list);
			REQUIRE_MESSAGE(drawn.has_value(), drawn.error().ToString());
			CHECK(counters.DrawCalls == 1);
			device.WaitForIdle();
		}
	}

}
