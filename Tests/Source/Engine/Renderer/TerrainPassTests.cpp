#include "TestsPCH.h"

#include "Engine/Renderer/TerrainPass.h"

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Fog.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SkyAtmosphere.h"
#include "Engine/Renderer/TerrainLod.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Shared/EnvironmentConstants.h"
#include "Support/BufferCreationFails.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

// The terrain passes (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 22, stream T-C; Renderer/TerrainPass.h):
// TerrainDepthNormal and TerrainForward driven by synthetic snapshots over in-memory heightfields. The GPU half of the
// acceptance "LOD seams are crack-free" ("SceneRenderer: a terrain whose chunks change level shows no gap") and the terrain's
// loss test of the recreation protocol (decision 29) live here, with MakeTerrainConstants' CPU case.

namespace Engine {

	// The terrain shader has a slot for every layer a Terrain holds (Renderer cannot include the component's header).
	static_assert(MaxTerrainShaderLayers == MaxTerrainLayers);

	namespace {

		constexpr AssetHandle HeightfieldHandle{ 0x9b01 };
		constexpr AssetHandle SplatHandle{ 0x9b02 };
		constexpr UUID TerrainEntity{ 0x7e44 };
		// The clear colour, which no terrain pixel of these scenes takes: a terrain shades its green or blue layers.
		constexpr glm::vec3 ClearColor{ 1.0f, 0.0f, 1.0f };

		// A bumpy heightfield of `resolution` samples a side: two crossing triangle waves of a few cells each, so every chunk's
		// edges climb and fall and a crack between two levels would open onto the background.
		AssetRef<Asset> MakeBumpyHeightfield(uint32_t resolution)
		{
			HeightfieldData heightfield = MakeFlatHeightfield(resolution);
			for (uint32_t z = 0; z < resolution; ++z)
			{
				for (uint32_t x = 0; x < resolution; ++x)
				{
					const int waveX = std::abs(static_cast<int>((x * 37) % 22) - 11);
					const int waveZ = std::abs(static_cast<int>((z * 23) % 18) - 9);
					heightfield.Heights[x + static_cast<size_t>(resolution) * z] = static_cast<uint16_t>(waveX * 3000 + waveZ * 3000);
				}
			}
			return CreateRef<HeightfieldData>(std::move(heightfield));
		}

		// A `width` x `height` RGBA8 data texture (Usage Linear, as splat maps are) whose texel (x, y) is `texel(x, y)`.
		template<typename TexelFunction>
		AssetRef<Asset> MakeSplatMap(uint32_t width, uint32_t height, const TexelFunction& texel)
		{
			TextureData texture;
			texture.Format = TextureFormat::RGBA8Unorm;
			texture.Width = width;
			texture.Height = height;
			texture.Mips.push_back({ .Width = width, .Height = height, .Offset = 0, .Size = static_cast<uint64_t>(width) * height * 4 });
			texture.Pixels.resize(static_cast<size_t>(width) * height * 4);
			for (uint32_t y = 0; y < height; ++y)
			{
				for (uint32_t x = 0; x < width; ++x)
				{
					const glm::u8vec4 value = texel(x, y);
					for (glm::length_t channel = 0; channel < 4; ++channel)
						texture.Pixels[(static_cast<size_t>(y) * width + x) * 4 + static_cast<size_t>(channel)] = std::byte{ value[channel] };
				}
			}
			return CreateRef<TextureData>(std::move(texture));
		}

		// The fixture's snapshot with the meshes and the pick table emptied, a camera at `eye` looking at `target` (a
		// perspective of `verticalFov` degrees, or an orthographic one `halfHeight` metres high), a sun, a grey ambient and the
		// magenta clear colour, and one terrain of `size` metres and `heightScale` over the published heightfield, two layers
		// (green and blue), no splat map.
		RenderSnapshot MakeTerrainSnapshot(Test::SceneRendererIntegrationFixture& fixture, RenderProjection projection, const glm::vec3& eye,
			const glm::vec3& target, float verticalFov, float halfHeight, float size, float heightScale)
		{
			RenderSnapshot snapshot = fixture.Snapshot(projection);
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			CameraData& camera = snapshot.Camera;
			camera.Position = eye;
			const glm::vec3 forward = glm::normalize(target - eye);
			const glm::vec3 up = std::abs(forward.y) > 0.99f ? glm::vec3(0.0f, 0.0f, -1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);
			camera.View = glm::lookAt(eye, target, up);
			camera.VerticalFov = verticalFov;
			camera.OrthographicSize = halfHeight;
			camera.NearClip = 0.1f;
			camera.FarClip = 1000.0f;
			camera.Projection = ComputeReverseZProjection(projection, verticalFov, halfHeight, 0.1f, 1000.0f, camera.ViewportWidth, camera.ViewportHeight);
			camera.ClearColor = ClearColor;
			snapshot.Lights = { LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(1.0f), .Intensity = 0.6f, .Direction = glm::normalize(glm::vec3(0.3f, -1.0f, 0.2f)), .CastShadows = false } };
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.2f) };
			snapshot.Post.ExposureEV = 0.0f;
			TerrainItem terrain;
			terrain.Heightfield = HeightfieldHandle;
			terrain.Size = glm::vec2(size);
			terrain.HeightScale = heightScale;
			terrain.Layers = { TerrainLayerItem{ .Tint = glm::vec3(0.2f, 0.7f, 0.25f), .Tiling = 3.0f, .Roughness = 0.8f },
				TerrainLayerItem{ .Tint = glm::vec3(0.15f, 0.2f, 0.8f), .Tiling = 5.0f, .Roughness = 0.4f } };
			terrain.Entity = TerrainEntity;
			terrain.PickId = 1;
			snapshot.PickTable = { TerrainEntity };
			snapshot.Terrains.push_back(std::move(terrain));
			return snapshot;
		}

		glm::ivec3 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			return { Test::SceneRendererIntegrationFixture::Channel(image, x, y, 0), Test::SceneRendererIntegrationFixture::Channel(image, x, y, 1),
				Test::SceneRendererIntegrationFixture::Channel(image, x, y, 2) };
		}

		// Records one render of `snapshot` and returns its status (the fixture's Submit requires success).
		Status SubmitAllowingErrors(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, const RenderSnapshot& snapshot)
		{
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			const Status rendered = fixture.Renderer().Render(**list, snapshot);
			(*list)->close();
			fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			return rendered;
		}

	}

	TEST_SUITE("Renderer")
	{
		// MakeTerrainConstants: the placement, the layers (one white one without any) and the flags.
		TEST_CASE("TerrainPass: a terrain's constants place its samples and pack its layers")
		{
			TerrainItem terrain;
			terrain.World = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(1.0f, 2.0f, 3.0f)), glm::vec3(-1.0f, 1.0f, 1.0f));
			terrain.Size = glm::vec2(64.0f, 32.0f);
			terrain.HeightScale = 10.0f;
			terrain.Layers = { TerrainLayerItem{ .Tint = glm::vec3(0.5f), .Tiling = 2.0f, .Roughness = 0.3f, .Metallic = 0.6f },
				TerrainLayerItem{ .Tiling = 0.0f, .Roughness = 2.0f, .Metallic = -1.0f } };
			terrain.ReceiveShadows = false;
			terrain.PickId = 7;
			const TerrainConstants reset = MakeTerrainConstants(terrain, 129, RenderDebugView::Albedo, true, 0b10);
			CHECK(reset.World == terrain.World);
			CHECK(reset.CellSize == glm::vec2(0.5f, 0.25f));
			CHECK(reset.InverseSize == glm::vec2(1.0f / 64.0f, 1.0f / 32.0f));
			CHECK(reset.HeightScale == 10.0f);
			CHECK(reset.Resolution == 129);
			CHECK(reset.LayerCount == 2);
			CHECK(reset.LayerTints[0] == glm::vec4(0.5f, 0.5f, 0.5f, 0.5f));
			CHECK(reset.LayerSurfaces[0] == glm::vec4(0.3f, 0.6f, 0.0f, 0.0f));
			// A Tiling that is not positive takes the default (4 m); Roughness and Metallic are clamped to [0, 1]; the second
			// layer's normal map is two-channel.
			CHECK(reset.LayerTints[1] == glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
			CHECK(reset.LayerSurfaces[1] == glm::vec4(1.0f, 0.0f, 1.0f, 0.0f));
			CHECK(reset.LayerTints[2] == glm::vec4(0.0f));
			CHECK(reset.Flags == (TerrainFlagSplatMap | TerrainFlagMirrored));
			CHECK(reset.PickId == 7);
			CHECK(reset.DebugView == static_cast<uint32_t>(RenderDebugView::Albedo));

			terrain.Layers.clear();
			terrain.World = glm::mat4(1.0f);
			terrain.ReceiveShadows = true;
			const TerrainConstants moved = MakeTerrainConstants(terrain, 33, RenderDebugView::Lit, false, 0);
			// Without layers, one white layer of TerrainLayerItem's defaults.
			CHECK(moved.LayerCount == 1);
			CHECK(moved.LayerTints[0] == glm::vec4(1.0f, 1.0f, 1.0f, 0.25f));
			CHECK(moved.LayerSurfaces[0] == glm::vec4(0.9f, 0.0f, 0.0f, 0.0f));
			CHECK(moved.Flags == TerrainFlagReceiveShadows);
			CHECK(moved.DebugView == static_cast<uint32_t>(RenderDebugView::Lit));
		}

		// The acceptance "LOD seams are crack-free" on the GPU: a bumpy terrain seen from just above, its chunks at several
		// levels (a small LodBias), shows the terrain at every pixel whose ray reaches the ground inside the terrain's extent: a
		// crack between two levels, or a pixel the forward pass's equal-depth test lost, would show the magenta clear colour.
		TEST_CASE("SceneRenderer: a terrain whose chunks change level shows no gap" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			constexpr uint32_t Width = 256;
			constexpr uint32_t Height = 160;
			Test::SceneRendererIntegrationFixture fixture(gpu, Width, Height);
			constexpr uint32_t Resolution = 257;
			fixture.Assets().Publish(HeightfieldHandle, MakeBumpyHeightfield(Resolution));
			constexpr float Size = 256.0f;
			constexpr float HeightScale = 6.0f;
			const glm::vec3 eye(128.0f, 9.0f, 200.0f);
			RenderSnapshot snapshot =
				MakeTerrainSnapshot(fixture, RenderProjection::Perspective, eye, glm::vec3(128.0f, 0.0f, 150.0f), 75.0f, 1.0f, Size, HeightScale);
			snapshot.Terrains.front().LodBias = 0.25f;

			// The levels the view sees differ: chunks near the camera are finer than those farther away.
			const TerrainLodLayout layout = ComputeTerrainLodLayout(Resolution);
			const std::vector<uint32_t> lods = SelectTerrainLods(layout, { .Size = glm::vec2(Size), .HeightScale = HeightScale }, eye, 0.25f);
			CHECK(std::set<uint32_t>(lods.begin(), lods.end()).size() >= 3);

			const Image image = fixture.Render(snapshot);
			const RenderStats stats = fixture.Renderer().GetRenderStats();
			const RenderPassStats* prepass = Test::SceneRendererIntegrationFixture::FindPass(stats, "TerrainDepthNormal");
			const RenderPassStats* forward = Test::SceneRendererIntegrationFixture::FindPass(stats, "TerrainForward");
			REQUIRE(prepass != nullptr);
			REQUIRE(forward != nullptr);
			CHECK(prepass->DrawCalls > 0);
			CHECK(forward->DrawCalls == prepass->DrawCalls);

			// Every pixel whose centre's ray meets the ground plane inside the terrain (with a margin from its edges) crosses the
			// terrain's surface first, since the camera is above the surface and inside its extent.
			constexpr float Margin = 2.0f;
			uint32_t inside = 0;
			uint32_t gaps = 0;
			for (uint32_t y = 0; y < Height; ++y)
			{
				for (uint32_t x = 0; x < Width; ++x)
				{
					const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
					REQUIRE(ray.has_value());
					if (ray->Direction.y >= -1e-3f)
						continue;
					const glm::vec3 ground = ray->Origin + ray->Direction * (-ray->Origin.y / ray->Direction.y);
					if (ground.x < Margin || ground.x > Size - Margin || ground.z < Margin || ground.z > Size - Margin)
						continue;
					++inside;
					if (GetPixel(image, x, y) == glm::ivec3(255, 0, 255))
						++gaps;
				}
			}
			CHECK(inside > Width * Height / 4); // most of the ground the view sees
			CHECK(gaps == 0);
		}

		// The splat map weighs the layers by its channels over their sum: the Albedo view of a terrain whose splat map is red
		// (layer 0) on its left half and green (layer 1) on its right shows the two layers' tints there.
		TEST_CASE("SceneRenderer: a terrain's splat map weighs its layers by their channels" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
			fixture.Assets().Publish(HeightfieldHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(33)));
			fixture.Assets().Publish(SplatHandle, MakeSplatMap(8, 8, [](uint32_t x, uint32_t /*y*/)
			{
				// Left half layer 0 (half weight each of red and alpha, alpha past the two layers weighing nothing), right half
				// layer 1.
				return x < 4 ? glm::u8vec4(128, 0, 0, 127) : glm::u8vec4(0, 255, 0, 0);
			}));
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Orthographic, glm::vec3(16.0f, 20.0f, 16.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 16.0f, 32.0f, 4.0f);
			snapshot.Terrains.front().SplatMap = SplatHandle;
			snapshot.DebugView = RenderDebugView::Albedo;
			const Image image = fixture.Render(snapshot);
			// The camera looks down -Y with -Z up on the image, so local +X is to the right: the left quarter is layer 0's green,
			// the right quarter layer 1's blue (sRGB-encoded albedos).
			const glm::ivec3 left = GetPixel(image, 8, 32);
			const glm::ivec3 right = GetPixel(image, 56, 32);
			CHECK(left.y > left.z);
			CHECK(left.y > left.x);
			CHECK(right.z > right.y);
			CHECK(right.z > right.x);
			// Without the splat map, layer 0 alone everywhere.
			snapshot.Terrains.front().SplatMap = AssetHandle();
			snapshot.FrameIndex += 1;
			const Image unsplatted = fixture.Render(snapshot);
			CHECK(GetPixel(unsplatted, 56, 32) == left);
			CHECK(GetPixel(unsplatted, 8, 32) == left);
		}

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 6): the terrain's forward pass fogs its radiance as the meshes' does.
		// A flat terrain seen straight down by an orthographic camera 20 m above it: each pixel's fogged radiance is its clear
		// radiance blended toward the fog's colour by the oracle's amount at the point the pixel's ray meets the ground.
		TEST_CASE("SceneRenderer: a terrain is fogged as a mesh is" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
			fixture.Assets().Publish(HeightfieldHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(33)));
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Orthographic, glm::vec3(16.0f, 20.0f, 16.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 16.0f, 32.0f, 4.0f);
			const auto readSceneColor = [&gpu, &fixture]()
			{
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*fixture.Renderer().GetSceneColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			};
			const auto radiance = [](const Image& image, uint32_t x, uint32_t y)
			{
				std::array<uint16_t, 4> halves{};
				std::memcpy(halves.data(), image.GetRow(y).data() + static_cast<size_t>(x) * sizeof(halves), sizeof(halves));
				return glm::dvec3(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]), Test::HalfToDouble(halves[2]));
			};
			fixture.Submit(snapshot);
			const Image clear = readSceneColor();
			snapshot.Fog = RenderFog{ .Enabled = true, .Color = glm::vec3(0.6f, 0.65f, 0.7f), .Density = 0.02f, .StartDistance = 2.0f, .MaxOpacity = 0.95f, .HeightDensity = 0.08f, .HeightFalloff = 0.3f, .BaseHeight = 1.0f };
			snapshot.FrameIndex += 1;
			fixture.Submit(snapshot);
			const Image fogged = readSceneColor();
			const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			uint32_t mismatched = 0;
			for (uint32_t y = 4; y < 60; ++y)
			{
				for (uint32_t x = 4; x < 60; ++x)
				{
					const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
					REQUIRE(ray.has_value());
					const glm::dvec3 origin(ray->Origin);
					const glm::dvec3 direction(ray->Direction);
					const glm::dvec4 reference = Test::EvaluateFog(fog, origin + direction * (-origin.y / direction.y));
					CHECK(reference.a > 0.3);
					const glm::dvec3 expected = radiance(clear, x, y) * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
					const glm::dvec3 actual = radiance(fogged, x, y);
					mismatched += glm::length(actual - expected) < 5e-3 + glm::length(expected) * 4e-3 ? 0U : 1U;
				}
			}
			CHECK(mismatched == 0);
		}

		// The picking prepass writes the terrain's pick id, and the stats count both terrain rows; a terrain whose heightfield is
		// missing draws nothing (reported once by the cache), and one with a world matrix that is not finite is a partial error
		// naming its entity while the others still draw.
		TEST_CASE("SceneRenderer: a terrain is picked by its pick id, and a missing heightfield or a non-finite world draws nothing"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
			fixture.Assets().Publish(HeightfieldHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(65, 1000)));
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Orthographic, glm::vec3(16.0f, 20.0f, 16.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 8.0f, 32.0f, 4.0f);
			snapshot.Flags = RenderViewFlags::Picking;
			fixture.Submit(snapshot);
			{
				Readback readback(gpu.GetDevice());
				const Result<Image> ids = readback.ReadTexture(*fixture.Renderer().GetEntityIdTexture());
				REQUIRE_MESSAGE(ids.has_value(), ids.error().ToString());
				uint32_t id = 0;
				std::memcpy(&id, ids->GetRow(32).data() + 32 * sizeof(id), sizeof(id));
				CHECK(id == 1);
			}

			// A second terrain far below the first with a heightfield nobody published, and a third with a NaN in its world.
			TerrainItem missing = snapshot.Terrains.front();
			missing.Heightfield = AssetHandle(0x9bff);
			missing.Entity = UUID(0x7e45);
			TerrainItem broken = snapshot.Terrains.front();
			broken.World[3].y = std::numeric_limits<float>::quiet_NaN();
			broken.Entity = UUID(0x7e46);
			snapshot.Terrains.push_back(missing);
			snapshot.Terrains.push_back(broken);
			snapshot.FrameIndex += 1;
			Status rendered;
			{
				Test::ExpectLog reported(LogLevel::Error, "ASSET_MISSING");
				rendered = SubmitAllowingErrors(gpu.GetDevice(), fixture, snapshot);
				CHECK(reported.GetMatchCount() == 1);
			}
			REQUIRE_FALSE(rendered.has_value());
			CHECK(rendered.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(rendered.error().ToString().contains(UUID(0x7e46).ToString()));
			const RenderStats stats = fixture.Renderer().GetRenderStats();
			const RenderPassStats* prepass = Test::SceneRendererIntegrationFixture::FindPass(stats, "TerrainDepthNormal");
			const RenderPassStats* forward = Test::SceneRendererIntegrationFixture::FindPass(stats, "TerrainForward");
			REQUIRE(prepass != nullptr);
			REQUIRE(forward != nullptr);
			// The first terrain alone: 65 samples are two chunks a side, all four in view.
			CHECK(prepass->DrawCalls == 4);
			CHECK(forward->DrawCalls == 4);
		}

		// TerrainDepthNormal's two M26 targets (decision 22): a terrain has no previous world, so its SceneVelocity is the
		// camera's motion alone, zero after a reset (no previous view-projection) and the projected shift of the camera's motion
		// otherwise; its SceneRoughness holds the forward pass's specular inputs, the sampled roughness in word 0's a and the
		// occlusion 1 (a terrain has no occlusion map) in word 1's x. Needs X-C's GetSceneVelocityTexture and
		// GetSceneRoughnessTexture (the W26.2 integration's tree).
		TEST_CASE("SceneRenderer: a terrain's prepass writes the camera's motion to SceneVelocity and its specular inputs to SceneRoughness"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
			fixture.Assets().Publish(HeightfieldHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(65, 1000)));
			// A flat terrain seen straight down by an orthographic camera 16 m across: 4 scene pixels per metre.
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Orthographic, glm::vec3(16.0f, 20.0f, 16.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 8.0f, 32.0f, 4.0f);
			const auto readMotion = [&gpu, &fixture](uint32_t x, uint32_t y)
			{
				nvrhi::ITexture* texture = fixture.Renderer().GetSceneVelocityTexture();
				REQUIRE(texture != nullptr);
				Readback readback(gpu.GetDevice());
				const Result<Image> image = readback.ReadTexture(*texture);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				std::array<uint16_t, 2> halves{};
				std::memcpy(halves.data(), image->GetRow(y).data() + static_cast<size_t>(x) * sizeof(halves), sizeof(halves));
				return glm::vec2(glm::unpackHalf1x16(halves[0]), glm::unpackHalf1x16(halves[1]));
			};
			fixture.Submit(snapshot);
			CHECK(readMotion(32, 32) == glm::vec2(0.0f));
			CHECK(readMotion(10, 50) == glm::vec2(0.0f));
			{
				Readback readback(gpu.GetDevice());
				const Result<Image> roughness = readback.ReadTexture(*fixture.Renderer().GetSceneRoughnessTexture());
				REQUIRE_MESSAGE(roughness.has_value(), roughness.error().ToString());
				std::array<uint32_t, 2> words{};
				std::memcpy(words.data(), roughness->GetRow(32).data() + 32 * sizeof(words), sizeof(words));
				// Layer 0's perceptual roughness 0.8 on a flat surface (no specular anti-aliasing widening): sqrt(alpha) = 0.8.
				CHECK(std::abs(static_cast<int>(words[0] >> 24) - 204) <= 1);
				CHECK(words[1] == 255U);
			}

			// The previous frame's camera 1 m further along -X: every point was 4 pixels further right then, so it moved 4 pixels
			// left (current minus previous).
			CameraData previous = snapshot.Camera;
			previous.View = glm::lookAt(glm::vec3(15.0f, 20.0f, 16.0f), glm::vec3(15.0f, 0.0f, 16.0f), glm::vec3(0.0f, 0.0f, -1.0f));
			snapshot.Camera.PreviousViewProjection = previous.Projection * previous.View;
			snapshot.FrameIndex += 1;
			fixture.Submit(snapshot);
			const glm::vec2 motion = readMotion(32, 32);
			CHECK(std::abs(motion.x - -4.0f) < 1e-2f);
			CHECK(std::abs(motion.y) < 1e-2f);
		}

		// A terrain's constant buffer or a chunk mesh's buffer that cannot be created (out of memory) leaves that terrain or chunk
		// out of the render, whose first error is the Gpu one; nothing is kept half-made, so the next render makes and draws them.
		TEST_CASE("SceneRenderer: a terrain buffer that cannot be created leaves its terrain or chunk out with a Gpu error, and the next render draws them"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 64, 64);
			fixture.Assets().Publish(HeightfieldHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(65, 1000)));
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Orthographic, glm::vec3(16.0f, 20.0f, 16.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 8.0f, 32.0f, 4.0f);
			// A first render without the terrain makes the view's own buffers, so the failures below meet the terrain's alone.
			RenderSnapshot empty = snapshot;
			empty.Terrains.clear();
			fixture.Submit(empty);
			const auto terrainPass = [&fixture](std::string_view row)
			{
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* pass = Test::SceneRendererIntegrationFixture::FindPass(stats, row);
				return pass != nullptr ? pass->DrawCalls : 0U;
			};
			{
				Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
				{
					return (info.usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) != 0 && info.size == sizeof(TerrainConstants);
				});
				snapshot.FrameIndex += 1;
				const Status rendered = SubmitAllowingErrors(gpu.GetDevice(), fixture, snapshot);
				CHECK(fails.GetFailedCount() == 1);
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
				CHECK(terrainPass("TerrainDepthNormal") == 0);
				CHECK(terrainPass("TerrainForward") == 0);
			}
			{
				Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
				{
					return (info.usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) != 0;
				});
				snapshot.FrameIndex += 1;
				const Status rendered = SubmitAllowingErrors(gpu.GetDevice(), fixture, snapshot);
				CHECK(fails.GetFailedCount() >= 1);
				REQUIRE_FALSE(rendered.has_value());
				CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
				CHECK(terrainPass("TerrainDepthNormal") == 0);
				CHECK(terrainPass("TerrainForward") == 0);
			}
			snapshot.FrameIndex += 1;
			fixture.Submit(snapshot);
			CHECK(terrainPass("TerrainDepthNormal") == 4);
			CHECK(terrainPass("TerrainForward") == 4);
		}

		// The recreation protocol (decision 29): a loss drops the pass's pipelines (place 1), the view's chunk meshes, constants
		// and sets (place 2) and the heightfield's and the layers' mirrors (place 3); the first render after the recovery
		// makes them again and renders the same image.
		TEST_CASE("SceneRenderer: a device loss recreates the terrain's chunks and textures and renders the same image" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			constexpr uint32_t ViewSize = 64;
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			fixture.Assets().Publish(HeightfieldHandle, MakeBumpyHeightfield(65));
			fixture.Assets().Publish(SplatHandle, MakeSplatMap(4, 4, [](uint32_t x, uint32_t y)
			{
				return (x + y) % 2 == 0 ? glm::u8vec4(255, 0, 0, 0) : glm::u8vec4(0, 255, 0, 0);
			}));
			RenderSnapshot snapshot = MakeTerrainSnapshot(fixture, RenderProjection::Perspective, glm::vec3(16.0f, 12.0f, 40.0f),
				glm::vec3(16.0f, 0.0f, 16.0f), 60.0f, 1.0f, 32.0f, 4.0f);
			snapshot.Terrains.front().SplatMap = SplatHandle;
			snapshot.Terrains.front().LodBias = 0.25f;
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 90;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex = ++frame;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			// The terrain filled the view's centre: not the magenta clear colour.
			REQUIRE(report->Before.size() == static_cast<size_t>(ViewSize) * ViewSize * 4);
			const size_t centre = (static_cast<size_t>(ViewSize / 2) * ViewSize + ViewSize / 2) * 4;
			CHECK(std::to_integer<int>(report->Before[centre + 1]) > 0);
		}
	}

}
