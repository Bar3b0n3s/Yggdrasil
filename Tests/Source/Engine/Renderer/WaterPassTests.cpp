#include "TestsPCH.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Physics/WaterWaves.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/TemporalHistory.h"
#include "Shared/WaterConstants.h"
#include "Support/BindingSetCreationFails.h"
#include "Support/BufferCreationFails.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

// The Water pass's GPU acceptance (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.1 record; Renderer/WaterPass.h).
// Each case renders synthetic snapshots through the scene renderer (Private/SceneRendererIntegrationFixture.h) and reads its
// targets back: SceneColor (linear HDR radiance), SceneVelocity, SceneRoughness and EntityId. The waters are built as render
// extraction builds them (Scene/RenderExtraction.cpp's MakeWaterItem): Physics/WaterWaves.h's ResolveWaterSurface and
// ComputeWaterWavePhases at the case's time, so the CPU surface the cases compare with is the one the queries sample.
// The render stream (W47.2) committed the seven acceptance cases skipped first, with their bodies, each recorded failing on
// C47's stubs (the W47.2 record), and lifted the skips with the pass.

namespace Engine {

	namespace {

		constexpr AssetHandle QuadMeshHandle{ 8910 }; // the fixture's 2 x 2 quad in local XY, facing +Z
		constexpr AssetHandle RedFloorMaterialHandle{ 0x5947000000000001ull };
		constexpr AssetHandle GreenMarkerMaterialHandle{ 0x5947000000000002ull };
		constexpr AssetHandle MirrorMaterialHandle{ 0x5947000000000003ull };
		constexpr AssetHandle GreyMaterialHandle{ 0x5947000000000004ull };
		constexpr uint32_t Size = 64;

		void PublishAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			Test::InMemoryAssetManager& assets = fixture.Assets();
			const auto emissive = [](const glm::vec3& color, float strength)
			{
				MaterialData material;
				material.BaseColor = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
				material.Roughness = 1.0f;
				material.Emissive = color;
				material.EmissiveStrength = strength;
				material.DoubleSided = true;
				return CreateRef<MaterialData>(std::move(material));
			};
			assets.Publish(RedFloorMaterialHandle, emissive(glm::vec3(1.0f, 0.0f, 0.0f), 1.0f));
			assets.Publish(GreenMarkerMaterialHandle, emissive(glm::vec3(0.0f, 1.0f, 0.0f), 2.0f));
			MaterialData mirror;
			mirror.BaseColor = glm::vec4(1.0f);
			mirror.Metallic = 1.0f;
			mirror.Roughness = 0.0f;
			assets.Publish(MirrorMaterialHandle, CreateRef<MaterialData>(std::move(mirror)));
			MaterialData grey;
			grey.BaseColor = glm::vec4(0.5f, 0.5f, 0.5f, 1.0f);
			grey.Roughness = 0.8f;
			assets.Publish(GreyMaterialHandle, CreateRef<MaterialData>(std::move(grey)));
		}

		// The fixture's quad turned to face +Y, scaled `size` along X and Z, centred on `centre`.
		glm::mat4 FloorWorld(const glm::vec3& centre, const glm::vec2& size)
		{
			return glm::translate(glm::mat4(1.0f), centre) * glm::rotate(glm::mat4(1.0f), glm::radians(-90.0f), glm::vec3(1.0f, 0.0f, 0.0f))
				* glm::scale(glm::mat4(1.0f), glm::vec3(0.5f * size.x, 0.5f * size.y, 1.0f));
		}

		// A water as render extraction draws it (Scene/RenderExtraction.cpp's MakeWaterItem): its waves resolved by
		// ResolveWaterSurface in the frame of `world` at a WaveScale of 1 and their phases at `time`, the component's default
		// look otherwise, and `pickId`.
		WaterItem MakeWater(RenderWaterShape shape, const glm::mat4& world, std::span<const WaterWave> waves, double time, uint64_t entity, uint32_t pickId)
		{
			const WaterSurfaceDesc surface = ResolveWaterSurface(waves, 1.0, glm::dvec2(0.0), MakeWaterFrame(glm::dmat4(world)));
			const std::array<double, MaxWaterSurfaceWaves> phases = ComputeWaterWavePhases(surface, time);
			WaterItem item;
			item.World = world;
			item.Shape = shape;
			item.WaveCount = surface.WaveCount;
			for (uint32_t index = 0; index < surface.WaveCount; ++index)
			{
				const WaterSurfaceWave& wave = surface.Waves[index];
				item.Waves[index] = WaterWaveItem{ .Direction = glm::vec2(wave.Direction),
					.WaveNumber = static_cast<float>(wave.WaveNumber),
					.Amplitude = static_cast<float>(wave.Amplitude),
					.Steepness = static_cast<float>(wave.Steepness),
					.Phase = static_cast<float>(phases[index]) };
			}
			item.Entity = UUID(entity);
			item.PickId = pickId;
			return item;
		}

		// The fixture's view with nothing in it, no light and no ambient (so a surface shows only what it reflects, refracts or
		// emits), the clear colour black, the Linear tonemapper, SSR and SSAO off, at `eye` looking at `target`.
		RenderSnapshot MakeView(Test::SceneRendererIntegrationFixture& fixture, RenderProjection projection, const glm::vec3& eye, const glm::vec3& target,
			const glm::vec3& up, float farClip)
		{
			RenderSnapshot snapshot = fixture.Snapshot(projection);
			snapshot.Meshes.clear();
			snapshot.PickTable.clear();
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
			snapshot.Post.SsrEnabled = false;
			snapshot.Camera.View = glm::lookAt(eye, target, up);
			snapshot.Camera.Position = eye;
			snapshot.Camera.FarClip = farClip;
			snapshot.Camera.Projection = ComputeReverseZProjection(projection, snapshot.Camera.VerticalFov, snapshot.Camera.OrthographicSize, snapshot.Camera.NearClip,
				farClip, Size, Size);
			return snapshot;
		}

		// Renders `snapshot` with a new frame index and returns Render's status.
		Status RenderStatus(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot& snapshot)
		{
			snapshot.FrameIndex += 1;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			Status rendered = fixture.Renderer().Render(**list, snapshot);
			(*list)->close();
			fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
			return rendered;
		}

		// A texture of the last render read back, row by row.
		Image ReadTexture(GraphicsDevice& device, nvrhi::ITexture* texture)
		{
			REQUIRE(texture != nullptr);
			Readback readback(device);
			Result<Image> image = readback.ReadTexture(*texture);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			return std::move(*image);
		}

		// The half-float texel `x`, `y` of a two- or four-channel binary16 image, channel by channel.
		glm::dvec4 HalfTexel(const Image& image, uint32_t x, uint32_t y, uint32_t channels)
		{
			std::array<uint16_t, 4> halves{};
			std::memcpy(halves.data(), image.GetRow(y).data() + static_cast<size_t>(x) * channels * sizeof(uint16_t), channels * sizeof(uint16_t));
			glm::dvec4 texel(0.0);
			for (uint32_t channel = 0; channel < channels; ++channel)
				texel[static_cast<glm::length_t>(channel)] = Test::HalfToDouble(halves[channel]);
			return texel;
		}

		// Renders `snapshot` (which must render without an error) and reads its SceneColor back: linear HDR radiance, row by row.
		std::vector<glm::dvec3> RenderSceneColor(GraphicsDevice& device, Test::SceneRendererIntegrationFixture& fixture, RenderSnapshot snapshot)
		{
			const Status rendered = RenderStatus(device, fixture, snapshot);
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			const Image image = ReadTexture(device, fixture.Renderer().GetSceneColorTexture());
			std::vector<glm::dvec3> texels;
			texels.reserve(static_cast<size_t>(Size) * Size);
			for (uint32_t y = 0; y < Size; ++y)
			{
				for (uint32_t x = 0; x < Size; ++x)
					texels.emplace_back(HalfTexel(image, x, y, 4));
			}
			return texels;
		}

		glm::dvec3 Texel(const std::vector<glm::dvec3>& texels, uint32_t x, uint32_t y)
		{
			return texels[static_cast<size_t>(y) * Size + x];
		}

		// A uint32_t of a 32-bit-per-channel image: channel `channel` of texel `x`, `y` of an image of `channels` channels.
		uint32_t WordTexel(const Image& image, uint32_t x, uint32_t y, uint32_t channels, uint32_t channel)
		{
			uint32_t word = 0;
			std::memcpy(&word, image.GetRow(y).data() + (static_cast<size_t>(x) * channels + channel) * sizeof(uint32_t), sizeof(word));
			return word;
		}

		// The scene pixel (top-left origin) world point `point` projects to through `viewProjection`.
		glm::vec2 ProjectToPixel(const glm::mat4& viewProjection, const glm::vec3& point)
		{
			const glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
			const glm::vec2 ndc = glm::vec2(clip) / clip.w;
			return glm::vec2((ndc.x * 0.5f + 0.5f) * static_cast<float>(Size), (0.5f - ndc.y * 0.5f) * static_cast<float>(Size));
		}

		// Three gentle Gerstner waves of a swell, for the surface comparison: sum Q k A about 0.05, well under the fold limit.
		constexpr std::array<WaterWave, 3> SwellWaves = {
			WaterWave{ .Direction = 20.0f, .Wavelength = 37.0f, .Amplitude = 0.25f, .Steepness = 0.6f, .Speed = 0.0f, .Phase = 30.0f },
			WaterWave{ .Direction = 110.0f, .Wavelength = 23.0f, .Amplitude = 0.08f, .Steepness = 0.5f, .Speed = 0.0f, .Phase = 200.0f },
			WaterWave{ .Direction = 250.0f, .Wavelength = 61.0f, .Amplitude = 0.3f, .Steepness = 0.3f, .Speed = 3.0f, .Phase = 75.0f },
		};

	}

	TEST_SUITE("Renderer")
	{
		// The rasterized surface's height at each pixel against the wave function's (Physics/WaterWaves.h's EvaluateWaterSurface,
		// which buoyancy, the scripts and water.sample read). An orthographic camera 7 m above a swell, 2 km from the water's
		// own origin, looks straight down at 4 x 4 m of it (64 x 64 pixels, all in the finest clipmap level). The snapshot's
		// previous view-projection is the current one sheared by the height (clip y + HeightShear (y - base)), and the water has
		// no previous pose, so each pixel's motion vector (SceneVelocity, current minus previous pixel) is 0.5 x 64 x HeightShear
		// pixels per metre of the surface point's height above the base: its y gives the drawn height (within binary16's
		// precision, about 0.3 mm here). The CPU height under the pixel's centre is within 1 mm of it at every pixel: the GPU's
		// waves (Common/Waves.slang, displacing the clipmap's vertices forward) are the CPU's, and the grid's linear
		// interpolation of this swell stays far under 1 mm in the finest level's 0.25 m cells.
		TEST_CASE("Water: the GPU surface equals the CPU surface within 1 mm" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				const glm::vec3 base(-300.0f, 1.0f, 450.0f);
				const glm::vec3 eye(1200.0f, 8.0f, -900.0f);
				const double time = 12.375;
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Orthographic, eye, eye - glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 20.0f);
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), base);
				snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, world, SwellWaves, time, 600, 0));
				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				constexpr float HeightShear = 0.125f; // 4 pixels of motion a metre
				glm::mat4 sheared = viewProjection;
				sheared[1][1] += HeightShear;
				sheared[3][1] -= HeightShear * base.y;
				snapshot.Camera.PreviousViewProjection = sheared;

				const Status rendered = RenderStatus(device, fixture, snapshot);
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				const Image velocity = ReadTexture(device, fixture.Renderer().GetSceneVelocityTexture());

				const WaterSurfaceDesc surface = ResolveWaterSurface(SwellWaves, 1.0, glm::dvec2(0.0), MakeWaterFrame(glm::dmat4(world)));
				const glm::mat4 inverse = glm::inverse(viewProjection);
				const double metresPerPixel = 1.0 / (0.5 * static_cast<double>(Size) * HeightShear);
				double worst = 0.0;
				uint32_t outside = 0;
				for (uint32_t y = 0; y < Size; ++y)
				{
					for (uint32_t x = 0; x < Size; ++x)
					{
						const glm::vec2 ndc((static_cast<float>(x) + 0.5f) / static_cast<float>(Size) * 2.0f - 1.0f,
							1.0f - (static_cast<float>(y) + 0.5f) / static_cast<float>(Size) * 2.0f);
						const glm::vec4 point = inverse * glm::vec4(ndc, 0.5f, 1.0f);
						const glm::dvec2 xz(static_cast<double>(point.x) / point.w, static_cast<double>(point.z) / point.w);
						const double expected = EvaluateWaterSurface(surface, xz, time).Height;
						const double drawn = static_cast<double>(base.y) + HalfTexel(velocity, x, y, 2).y * metresPerPixel;
						const double error = std::abs(drawn - expected);
						worst = std::max(worst, error);
						if (error > 1.0e-3)
							++outside;
					}
				}
				INFO("the largest difference between the drawn and the CPU surface: " << worst * 1000.0 << " mm");
				CHECK(outside == 0);
			}
			device.RunGarbageCollection();
		}

		// A scene without water records nothing of the water: no Water or WaterRefraction row, no depth pyramid for it (SSAO
		// and SSR off) and no surfaces counted, and renders the same bytes before and after a render that drew a water (which
		// changes the image and records the rows and the pyramid), so the water leaves no state that reaches a later render.
		TEST_CASE("Water: a scene without water renders the same bytes" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot plain = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 2.0f, 4.0f), glm::vec3(0.0f, 0.0f, -2.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				plain.Environment.FallbackColor = glm::vec3(0.3f);
				plain.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -0.5f, -2.0f), glm::vec2(8.0f)), .Mesh = QuadMeshHandle, .Materials = { GreyMaterialHandle }, .Entity = UUID(1) });
				plain.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.0f, -4.0f)), .Mesh = QuadMeshHandle, .Materials = { GreenMarkerMaterialHandle }, .Entity = UUID(2) });
				const auto hasRow = [&fixture](std::string_view row)
				{
					return Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), row) != nullptr;
				};

				const Image before = fixture.Render(plain);
				CHECK_FALSE(hasRow("WaterRefraction"));
				CHECK_FALSE(hasRow("Water"));
				CHECK_FALSE(hasRow("DepthPyramid"));
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);

				RenderSnapshot watered = plain;
				watered.FrameIndex += 1;
				watered.Waters.push_back(MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2.0f)), SwellWaves, 3.0, 600, 0));
				const Image drawn = fixture.Render(watered);
				CHECK(drawn.Pixels != before.Pixels);
				CHECK(hasRow("WaterRefraction"));
				CHECK(hasRow("Water"));
				CHECK(hasRow("DepthPyramid"));
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);

				plain.FrameIndex = watered.FrameIndex + 1;
				const Image after = fixture.Render(plain);
				CHECK(after.Pixels == before.Pixels);
				CHECK_FALSE(hasRow("Water"));
				CHECK_FALSE(hasRow("DepthPyramid"));
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);
			}
			device.RunGarbageCollection();
		}

		// A camera 5 m above calm water looks straight down at a red emitting floor: 0.2 m under the surface on the left, 30 m
		// under it on the right. Without light or ambient, and with no foam and no refraction offset, the water shows only what it
		// refracts, absorbed by Beer-Lambert over the water column (Clarity 4 m): the shallows let exp(-0.2 / 4), about 95 %, of
		// the floor through (less the little the surface reflects), the deep water about 0.06 %. Without the water both halves
		// show the floor.
		TEST_CASE("Water: shallow water shows the bottom and deep water absorbs it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(-20.0f, -0.2f, 0.0f), glm::vec2(40.0f)), .Mesh = QuadMeshHandle, .Materials = { RedFloorMaterialHandle }, .Entity = UUID(1) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(20.0f, -30.0f, 0.0f), glm::vec2(40.0f)), .Mesh = QuadMeshHandle, .Materials = { RedFloorMaterialHandle }, .Entity = UUID(2) });
				const std::vector<glm::dvec3> dry = RenderSceneColor(device, fixture, snapshot);
				CHECK(Texel(dry, 16, 32).r > 0.95);
				CHECK(Texel(dry, 48, 32).r > 0.95);

				WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.FoamAmount = 0.0f;
				water.RefractionStrength = 0.0f;
				snapshot.Waters.push_back(water);
				const std::vector<glm::dvec3> wet = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				const glm::dvec3 shallow = Texel(wet, 16, 32);
				const glm::dvec3 deep = Texel(wet, 48, 32);
				INFO("shallow " << shallow.r << ", deep " << deep.r);
				CHECK(shallow.r > 0.85);
				CHECK(shallow.r < 0.97);
				CHECK(deep.r < 0.01);
			}
			device.RunGarbageCollection();
		}

		// A camera 1 m above calm, smooth water looks along it at a green emitting marker standing on it 6 m ahead; the scene
		// has no light and no ambient, so the water's specular term reflects black and only SSR shows the marker in it. With SSR
		// the marker's mirror image in the water (the marker's centre reflected in the base plane) is green; without SSR it is
		// black. The water flags its pixels in SceneRoughness (word 1's y byte, so the composite takes the water's own occlusion)
		// and writes a non-zero specular multiplier there (word 0), so SSR traces them.
		TEST_CASE("Water: SSR traces and composites over the water" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 1.0f, 2.0f), glm::vec3(0.0f, 0.2f, -4.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				const glm::vec3 markerCentre(0.0f, 1.2f, -6.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), markerCentre), .Mesh = QuadMeshHandle, .Materials = { GreenMarkerMaterialHandle }, .Entity = UUID(1) });
				WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.Roughness = 0.02f;
				water.DetailNormalStrength = 0.0f;
				water.FoamAmount = 0.0f;
				snapshot.Waters.push_back(water);
				snapshot.Post.SsrMaxRoughness = 0.6f;

				const std::vector<glm::dvec3> off = RenderSceneColor(device, fixture, snapshot);
				snapshot.Post.SsrEnabled = true;
				const std::vector<glm::dvec3> on = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "SSR") != nullptr);

				const glm::vec2 image = ProjectToPixel(snapshot.Camera.Projection * snapshot.Camera.View, glm::vec3(markerCentre.x, -markerCentre.y, markerCentre.z));
				const auto x = static_cast<uint32_t>(image.x);
				const auto y = static_cast<uint32_t>(image.y);
				REQUIRE(x < Size);
				REQUIRE(y < Size);
				INFO("the reflection's pixel (" << x << ", " << y << "): green " << Texel(off, x, y).g << " without SSR, " << Texel(on, x, y).g << " with it");
				CHECK(Texel(off, x, y).g < 0.02);
				CHECK(Texel(on, x, y).g > 0.1);
				CHECK(Texel(on, x, y).g > Texel(off, x, y).g + 0.08);

				const Image roughness = ReadTexture(device, fixture.Renderer().GetSceneRoughnessTexture());
				CHECK((WordTexel(roughness, x, y, 2, 0) & 0x00FFFFFFU) != 0);
				CHECK(((WordTexel(roughness, x, y, 2, 1) >> 8) & 0xFFU) == 0xFFU);
				// The marker's own pixels are not water.
				const glm::vec2 marker = ProjectToPixel(snapshot.Camera.Projection * snapshot.Camera.View, markerCentre);
				CHECK(((WordTexel(roughness, static_cast<uint32_t>(marker.x), static_cast<uint32_t>(marker.y), 2, 1) >> 8) & 0xFFU) == 0);
			}
			device.RunGarbageCollection();
		}

		// A camera 10 m above an Ocean at height 0 looks down at a 4 x 4 m Rectangle whose base lies 1 m below the sea's (a cave
		// lake under the sea's level). With a Headroom of 5 m its volume reaches above the sea, so inside its footprint the Ocean
		// writes zero coverage and the picking view's EntityId shows the lake; outside it, the sea. With a Headroom of 0.5 m the
		// volume ends under the sea's surface, which is then drawn over the lake inside the footprint too.
		TEST_CASE("Water: a Rectangle's volume masks the Ocean" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
				snapshot.Flags = RenderViewFlags::Picking;
				snapshot.PickTable = { UUID(601), UUID(602) };
				snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 601, 1));
				WaterItem lake = MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)), {}, 0.0, 602, 2);
				lake.Size = glm::vec2(4.0f);
				lake.Headroom = 5.0f;
				snapshot.Waters.push_back(lake);
				const auto pickIdAt = [&device, &fixture](uint32_t x, uint32_t y)
				{
					const Image ids = ReadTexture(device, fixture.Renderer().GetEntityIdTexture());
					return WordTexel(ids, x, y, 1, 0);
				};

				// The view spans 2 tan 30 degrees x 10 m = 11.5 m at the sea: the footprint's 4 m are pixels 21 to 42.
				const Status masked = RenderStatus(device, fixture, snapshot);
				REQUIRE_MESSAGE(masked.has_value(), masked.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 2);
				CHECK(pickIdAt(32, 32) == 2);
				CHECK(pickIdAt(25, 38) == 2);
				CHECK(pickIdAt(4, 32) == 1);
				CHECK(pickIdAt(60, 5) == 1);

				snapshot.Waters[1].Headroom = 0.5f;
				const Status covered = RenderStatus(device, fixture, snapshot);
				REQUIRE_MESSAGE(covered.has_value(), covered.error().ToString());
				CHECK(pickIdAt(32, 32) == 1);
				CHECK(pickIdAt(25, 38) == 1);
				CHECK(pickIdAt(4, 32) == 1);
			}
			device.RunGarbageCollection();
		}

		// The sea is out of M42's mirror views and of probe bakes (decision 3). A mirror stands above an Ocean 1.5 m under the
		// camera's eye, facing it, its glass from 0.4 m below eye level to 1.6 m above: its mirror view would see the sea's
		// surface in front of the glass in the glass's lower rows, but the glass shows the same bytes with the Ocean in the
		// snapshot as without it, and its mirror view records as many draws, while the main view draws the sea below the glass
		// (whose crests stay under its lower edge). A probe captured over the sea gives the same texels with the Ocean as
		// without it.
		TEST_CASE("Water: mirror views and probe captures leave the water out" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot dry = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				dry.Camera.ClearColor = glm::vec3(0.1f, 0.2f, 0.3f);
				dry.Environment.FallbackColor = glm::vec3(0.2f);
				// The glass from y = -0.4 to 1.6 at z = -3, facing +Z.
				const glm::mat4 glass = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.6f, -3.0f));
				dry.Meshes.push_back(MeshDrawItem{ .World = glass, .Mesh = QuadMeshHandle, .Materials = { MirrorMaterialHandle }, .CastShadows = false, .Entity = UUID(900) });
				dry.PlanarReflections.push_back(PlanarReflectionItem{ .World = glass, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Entity = UUID(900) });
				dry.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.3f)),
					.Mesh = QuadMeshHandle,
					.Materials = { GreenMarkerMaterialHandle },
					.Entity = UUID(1) });
				RenderSnapshot wet = dry;
				wet.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.5f, 0.0f)), SwellWaves, 1.0, 600, 0));

				const std::vector<glm::dvec3> withoutSea = RenderSceneColor(device, fixture, dry);
				const RenderStats withoutStats = fixture.Renderer().GetRenderStats();
				const std::vector<glm::dvec3> withSea = RenderSceneColor(device, fixture, wet);
				const RenderStats withStats = fixture.Renderer().GetRenderStats();
				CHECK(withStats.WaterSurfaces == 1);
				CHECK(withStats.PlanarReflections == 1);
				const RenderPassStats* withMirror = Test::SceneRendererIntegrationFixture::FindPass(withStats, "PlanarReflection0");
				const RenderPassStats* withoutMirror = Test::SceneRendererIntegrationFixture::FindPass(withoutStats, "PlanarReflection0");
				REQUIRE(withMirror != nullptr);
				REQUIRE(withoutMirror != nullptr);
				CHECK(withMirror->DrawCalls == withoutMirror->DrawCalls);
				// The glass covers columns 14 to 50 and, from y = 1.6 to -0.4 at 3 m, rows 3 to 39: two pixels in from its edges.
				uint32_t glassDifferences = 0;
				for (uint32_t y = 5; y < 38; ++y)
				{
					for (uint32_t x = 16; x < 49; ++x)
						glassDifferences += Texel(withSea, x, y) != Texel(withoutSea, x, y) ? 1U : 0U;
				}
				CHECK(glassDifferences == 0);
				// Below the glass the main view draws the sea.
				uint32_t seaDifferences = 0;
				for (uint32_t x = 0; x < Size; ++x)
					seaDifferences += Texel(withSea, x, 60) != Texel(withoutSea, x, 60) ? 1U : 0U;
				CHECK(seaDifferences == Size);

				Result<Scope<ProbeCapture>> capture = ProbeCapture::Create(device, fixture.Pipelines(), fixture.Cache(), fixture.Assets());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				const glm::vec3 probe(0.0f, 1.0f, 0.0f);
				const Result<ProbeCubeCapture> plain = (*capture)->Capture(dry, probe, 16);
				REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
				const Result<ProbeCubeCapture> overSea = (*capture)->Capture(wet, probe, 16);
				REQUIRE_MESSAGE(overSea.has_value(), overSea.error().ToString());
				for (size_t face = 0; face < plain->Faces.size(); ++face)
					CHECK(overSea->Faces[face] == plain->Faces[face]);
			}
			device.RunGarbageCollection();
		}

		// The pass's pipelines and ring meshes live in the pipeline set (place 1 of Architecture §8.14 item 10) and its records,
		// binding sets and WaterRefraction in the renderer's state and pool (place 2): the recovery releases and creates them
		// again, and a scene of a swell over a floor renders the same bytes.
		TEST_CASE("Water: a water scene renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
			PublishAssets(fixture);
			RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -3.0f),
				glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
			snapshot.Environment.FallbackColor = glm::vec3(0.4f, 0.45f, 0.5f);
			snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -1.5f, -3.0f), glm::vec2(10.0f)), .Mesh = QuadMeshHandle, .Materials = { GreyMaterialHandle }, .Entity = UUID(1) });
			snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, 4.5, 600, 0));
			GraphicsDevice& device = gpu.GetDevice();
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex += 1;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					if (fixture.Renderer().GetRenderStats().WaterSurfaces != 1)
						return MakeError(ErrorCode::InvalidState, "the water was not drawn");
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
		}

		// The motion vectors through the view's history (Renderer/TemporalHistory.h fills a water's previous pose and phases by
		// its entity): a still water under a still camera has exactly zero motion at every pixel; waves a moment later move its
		// pixels; a tide that raises it (its pose, at the same waves) moves them up the image, as a camera looking down at it
		// sees.
		TEST_CASE("Water: the motion vectors follow the waves and the water's pose across frames, and a still water has none"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				RenderViewHistory history;
				const auto render = [&device, &fixture, &history](const RenderSnapshot& source)
				{
					RenderSnapshot snapshot = source;
					history.Apply(snapshot, 0.0f);
					const Status rendered = RenderStatus(device, fixture, snapshot);
					REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
					CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
					const Image velocity = ReadTexture(device, fixture.Renderer().GetSceneVelocityTexture());
					glm::dvec2 sum(0.0);
					uint32_t moving = 0;
					for (uint32_t y = 0; y < Size; ++y)
					{
						for (uint32_t x = 0; x < Size; ++x)
						{
							const glm::dvec2 motion(HalfTexel(velocity, x, y, 2));
							sum += motion;
							moving += motion != glm::dvec2(0.0) ? 1U : 0U;
						}
					}
					return std::pair{ sum / static_cast<double>(Size * Size), moving };
				};
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 4.0f, 3.0f), glm::vec3(0.0f, 0.0f, -4.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				snapshot.Camera.Entity = UUID(77);
				snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, 2.0, 600, 0));

				static_cast<void>(render(snapshot));
				const auto [stillMean, still] = render(snapshot);
				CHECK(still == 0);

				snapshot.Waters[0] = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, 2.25, 600, 0);
				const auto [wavingMean, waving] = render(snapshot);
				CHECK(waving > Size * Size / 2);

				snapshot.Waters[0] = MakeWater(RenderWaterShape::Ocean, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, 0.0f)), SwellWaves, 2.25, 600, 0);
				const auto [risingMean, rising] = render(snapshot);
				CHECK(rising > Size * Size / 2);
				// Up the image is -y in scene pixels.
				CHECK(risingMean.y < -0.5);
			}
			device.RunGarbageCollection();
		}

		// Out of memory (Architecture §8.14 item 7, the per-render rule of ADR 0029 decision 44). Under oom-rendertarget the first
		// render target created after the fault is armed is the render's WaterRefraction (the depth pyramid's and SSR's targets
		// exist since the first render), so that render draws no water: the same bytes as the scene without it, no Water row,
		// no surface counted, and the Gpu error returned, as a host trims after the frame. In the next render the water's set
		// cannot be created: it draws no water either (its copy of SceneColor changes nothing) and returns the error; the render
		// after it draws the water. A second water outgrows the view's records, whose buffer then cannot be created: that render
		// draws neither water and returns the error, and the next render draws both.
		TEST_CASE("Water: a water whose refraction, set or records cannot be created is left out of the render, which returns the error, and the next render draws it"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::OomRenderTarget });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot dry = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f, 1.0f, 0.0f),
					100.0f);
				dry.Environment.FallbackColor = glm::vec3(0.3f);
				dry.Post.SsrEnabled = true;
				dry.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -1.0f, -3.0f), glm::vec2(10.0f)), .Mesh = QuadMeshHandle, .Materials = { GreyMaterialHandle }, .Entity = UUID(1) });
				const Image without = fixture.Render(dry);
				device.RunGarbageCollection(); // the first frame's collection arms the fault

				RenderSnapshot wet = dry;
				wet.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, 1.0, 600, 0));
				const Status failed = RenderStatus(device, fixture, wet);
				REQUIRE_FALSE(failed.has_value());
				CHECK(failed.error().GetCode() == ErrorCode::Gpu);
				CHECK_FALSE(device.IsLost());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "Water") == nullptr);
				CHECK(fixture.Read().Pixels == without.Pixels);

				Status unbound;
				{
					// The water's set 1: its records, the refraction, the pyramid and its two maps (Renderer/WaterPass.h).
					const Test::BindingSetCreationFails fails(device, [](const nvrhi::BindingSetDesc& desc, const nvrhi::IBindingLayout& /*layout*/)
					{
						return desc.bindings.size() == 5 && desc.bindings[0].type == nvrhi::ResourceType::StructuredBuffer_SRV;
					});
					unbound = RenderStatus(device, fixture, wet);
					CHECK(fails.GetFailedCount() == 1);
				}
				CHECK_FALSE(unbound.has_value());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);
				CHECK(fixture.Read().Pixels == without.Pixels);

				const Status recovered = RenderStatus(device, fixture, wet);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				CHECK(fixture.Read().Pixels != without.Pixels);

				WaterItem pool = MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.5f, -3.0f)), {}, 0.0, 601, 0);
				pool.Size = glm::vec2(2.0f);
				wet.Waters.push_back(pool);
				Status outgrown;
				{
					// The view's records for two waters are the only storage buffer of their size the render creates.
					const Test::BufferCreationFails fails([](const VkBufferCreateInfo& info)
					{
						return info.size == 2 * sizeof(WaterRecord) && (info.usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) != 0;
					});
					outgrown = RenderStatus(device, fixture, wet);
					CHECK(fails.GetFailedCount() == 1);
				}
				CHECK_FALSE(outgrown.has_value());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);
				CHECK(fixture.Read().Pixels == without.Pixels);
				const Status grown = RenderStatus(device, fixture, wet);
				REQUIRE_MESSAGE(grown.has_value(), grown.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 2);
			}
			device.RunGarbageCollection();
		}

		// The W47.2 review's finding 1: the refraction is SceneColor's, which the floor's pass already fogged over the whole path
		// from the camera to the floor, so the water fogs only its own terms, the fog's colour weighted by its opacity. A camera
		// 5 m above calm, unlit water (black environment, no light, no foam, no refraction offset) looks straight down at a red
		// emitting floor 0.2 m under it, in a blue fog of 0.07 a metre. The water's own terms are black, so it shows the dry
		// render's fogged floor times its transmission (about 93 %: exp(-0.2 / 4) less the little the surface reflects), plus
		// the fog's colour at the surface times the water's opacity (the rest); fogging the refraction again would darken the
		// red by the fog's amount (about 30 %) and add the fog's colour a second time.
		TEST_CASE("Water: in a fogged view the refracted bottom is fogged once, as the dry bottom is" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
				constexpr float Density = 0.07f;
				snapshot.Fog = RenderFog{ .Enabled = true, .Color = glm::vec3(0.0f, 0.0f, 1.0f), .Density = Density, .MaxOpacity = 1.0f, .SunGlow = 0.0f };
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -0.2f, 0.0f), glm::vec2(40.0f)), .Mesh = QuadMeshHandle, .Materials = { RedFloorMaterialHandle }, .Entity = UUID(1) });
				const std::vector<glm::dvec3> dry = RenderSceneColor(device, fixture, snapshot);

				WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.FoamAmount = 0.0f;
				water.RefractionStrength = 0.0f;
				water.DetailNormalStrength = 0.0f;
				snapshot.Waters.push_back(water);
				const std::vector<glm::dvec3> wet = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);

				// The fog's amount at the surface: 5 m of view depth at every pixel of the plane facing the camera.
				const double surfaceFog = 1.0 - std::exp(-static_cast<double>(Density) * 5.0);
				for (const glm::uvec2 pixel : { glm::uvec2(32, 32), glm::uvec2(12, 20), glm::uvec2(50, 44) })
				{
					const glm::dvec3 before = Texel(dry, pixel.x, pixel.y);
					const glm::dvec3 after = Texel(wet, pixel.x, pixel.y);
					INFO("pixel (" << pixel.x << ", " << pixel.y << "): dry " << before.r << ", " << before.b << "; wet " << after.r << ", " << after.b);
					REQUIRE(before.r > 0.6);
					REQUIRE(before.b > 0.25);
					const double transmission = after.r / before.r;
					CHECK(transmission > 0.9);
					CHECK(transmission < 0.97);
					CHECK(std::abs(after.b - (before.b * transmission + surfaceFog * (1.0 - transmission))) < 0.01);
				}
			}
			device.RunGarbageCollection();
		}

		// The W47.2 review's finding 2: SSR's half-resolution reconstruction weighs its taps by depth, and the depth pyramid,
		// recorded before the water, holds the seabed's depth at a water pixel. An orthographic camera looks down at 30 degrees
		// at calm, smooth water reflecting a green emitting wall; under the water lies a floor 3 m deep and, over it, a ridge
		// 1 m deep that covers exactly one even row of the image. Weighed by the seabed's depth, that row's four taps (on the
		// rows above and below it, over the floor) would all be rejected and the row would lose its reflection, a seam
		// outlining the ridge; weighed by the water's own depth (SceneDepth), the row reflects the wall as the full-resolution
		// trace does.
		TEST_CASE("Water: a half-resolution trace weighs a water pixel's taps by the water's depth, so a ridge under it leaves no seam"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const glm::vec3 target(0.0f, 0.0f, -2.0f);
				const glm::vec3 forward(0.0f, -0.5f, -std::sqrt(0.75f)); // 30 degrees down
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Orthographic, target - forward * 50.0f, target, glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				snapshot.Camera.OrthographicSize = 4.0f;
				snapshot.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, snapshot.Camera.VerticalFov, snapshot.Camera.OrthographicSize,
					snapshot.Camera.NearClip, snapshot.Camera.FarClip, Size, Size);
				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				// The ridge's plane, y = -1: a point's image row is affine in its z. The ridge spans rows Row - 0.3 to Row + 1.3,
				// so only Row's pixel centres lie on it.
				constexpr uint32_t Row = 24;
				const float rowAtZero = ProjectToPixel(viewProjection, glm::vec3(0.0f, -1.0f, 0.0f)).y;
				const float rowsPerMetre = ProjectToPixel(viewProjection, glm::vec3(0.0f, -1.0f, 1.0f)).y - rowAtZero;
				const float nearZ = (static_cast<float>(Row) + 1.3f - rowAtZero) / rowsPerMetre;
				const float farZ = (static_cast<float>(Row) - 0.3f - rowAtZero) / rowsPerMetre;
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -3.0f, -2.0f), glm::vec2(40.0f)), .Mesh = QuadMeshHandle, .Materials = { GreyMaterialHandle }, .Entity = UUID(1) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -1.0f, 0.5f * (nearZ + farZ)), glm::vec2(40.0f, std::abs(nearZ - farZ))),
					.Mesh = QuadMeshHandle,
					.Materials = { GreyMaterialHandle },
					.Entity = UUID(2) });
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 3.0f, -6.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 3.0f, 1.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { GreenMarkerMaterialHandle },
					.Entity = UUID(3) });
				WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.Roughness = 0.02f;
				water.DetailNormalStrength = 0.0f;
				water.FoamAmount = 0.0f;
				water.RefractionStrength = 0.0f;
				snapshot.Waters.push_back(water);
				snapshot.Post.SsrEnabled = true;

				const std::vector<glm::dvec3> full = RenderSceneColor(device, fixture, snapshot);
				snapshot.Quality.SsrHalfResolution = true;
				const std::vector<glm::dvec3> half = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				uint32_t compared = 0;
				uint32_t seam = 0;
				for (uint32_t x = 8; x < Size - 8; ++x)
				{
					const double reflected = Texel(full, x, Row).g;
					REQUIRE(reflected > 0.1);
					++compared;
					seam += Texel(half, x, Row).g < 0.5 * reflected ? 1U : 0U;
				}
				INFO("row " << Row << ": " << seam << " of " << compared << " pixels lose the reflection at half resolution");
				CHECK(seam == 0);
			}
			device.RunGarbageCollection();
		}

		// The W47.2 review's finding 3: a Rectangle draws the rings clamped to its footprint, so a calm one draws exactly its
		// footprint. A camera 10 m above a calm Rectangle, turned 30 degrees and scaled 1.5 along its X and 0.75 along its Z
		// (6 x 3 m from a Size of 4 x 4), picks the lake 1.5 pixels inside the midpoint of each of its four edges and nothing
		// 1.5 pixels outside.
		TEST_CASE("Water: a calm Rectangle draws exactly its footprint, turned and scaled" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.0f, 0.0f, 0.0f),
					glm::vec3(0.0f, 0.0f, -1.0f), 100.0f);
				snapshot.Flags = RenderViewFlags::Picking;
				snapshot.PickTable = { UUID(602) };
				const glm::vec3 centre(0.4f, 0.0f, -0.3f);
				const glm::mat4 world = glm::translate(glm::mat4(1.0f), centre) * glm::rotate(glm::mat4(1.0f), glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::scale(glm::mat4(1.0f), glm::vec3(1.5f, 1.0f, 0.75f));
				WaterItem lake = MakeWater(RenderWaterShape::Rectangle, world, {}, 0.0, 602, 1);
				lake.Size = glm::vec2(4.0f);
				snapshot.Waters.push_back(lake);
				const Status rendered = RenderStatus(device, fixture, snapshot);
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
				const Image ids = ReadTexture(device, fixture.Renderer().GetEntityIdTexture());
				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				const glm::vec2 centrePixel = ProjectToPixel(viewProjection, centre);
				const glm::vec3 axisX = glm::vec3(world[0]) * 2.0f; // half the Size times the column: 3 m along the turned X
				const glm::vec3 axisZ = glm::vec3(world[2]) * 2.0f; // 1.5 m along the turned Z
				for (const glm::vec3 edge : { axisX, -axisX, axisZ, -axisZ })
				{
					const glm::vec2 midpoint = ProjectToPixel(viewProjection, centre + edge);
					const glm::vec2 inward = glm::normalize(centrePixel - midpoint);
					const glm::vec2 inside = midpoint + inward * 1.5f;
					const glm::vec2 outside = midpoint - inward * 1.5f;
					INFO("the edge's midpoint at pixel (" << midpoint.x << ", " << midpoint.y << ")");
					REQUIRE(outside.x >= 0.0f);
					REQUIRE(outside.y >= 0.0f);
					REQUIRE(outside.x < static_cast<float>(Size));
					REQUIRE(outside.y < static_cast<float>(Size));
					CHECK(WordTexel(ids, static_cast<uint32_t>(inside.x), static_cast<uint32_t>(inside.y), 1, 0) == 1);
					CHECK(WordTexel(ids, static_cast<uint32_t>(outside.x), static_cast<uint32_t>(outside.y), 1, 0) == 0);
				}
			}
			device.RunGarbageCollection();
		}

		// The W47.2 review's finding 4 (decision 3: the data views, the debug views and the wireframe draw no water): a scene
		// with an Ocean, SSAO and SSR off, rendered in the Albedo view, the LightCount data view and the wireframe, records
		// neither the Water nor the WaterRefraction row, counts no surface and records no depth pyramid; its Lit view draws
		// the water.
		TEST_CASE("Water: the debug views, the data views and the wireframe draw no water" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot lit = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 2.0f, 3.0f), glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f, 1.0f, 0.0f),
					100.0f);
				lit.Environment.FallbackColor = glm::vec3(0.3f);
				lit.Post.SsaoEnabled = false;
				lit.Meshes.push_back(MeshDrawItem{ .World = FloorWorld(glm::vec3(0.0f, -1.0f, -3.0f), glm::vec2(10.0f)), .Mesh = QuadMeshHandle, .Materials = { GreyMaterialHandle }, .Entity = UUID(1) });
				lit.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, 1.0, 600, 0));
				const auto hasRow = [&fixture](std::string_view row)
				{
					return Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), row) != nullptr;
				};
				RenderSnapshot albedo = lit;
				albedo.DebugView = RenderDebugView::Albedo;
				RenderSnapshot lightCount = lit;
				lightCount.DebugView = RenderDebugView::LightCount;
				RenderSnapshot wireframe = lit;
				wireframe.Flags = RenderViewFlags::Wireframe;
				for (RenderSnapshot* snapshot : { &albedo, &lightCount, &wireframe })
				{
					CAPTURE(static_cast<int>(snapshot->DebugView));
					CAPTURE(snapshot == &wireframe);
					const Status rendered = RenderStatus(device, fixture, *snapshot);
					REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
					CHECK_FALSE(hasRow("Water"));
					CHECK_FALSE(hasRow("WaterRefraction"));
					CHECK_FALSE(hasRow("DepthPyramid"));
					CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 0);
				}
				const Status rendered = RenderStatus(device, fixture, lit);
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
				CHECK(hasRow("Water"));
				CHECK(hasRow("WaterRefraction"));
				CHECK(fixture.Renderer().GetRenderStats().WaterSurfaces == 1);
			}
			device.RunGarbageCollection();
		}

		// The planar opt-in (decision 3, W47.3): a PlanarReflection on a water gives an item of its base plane (Normal +Y,
		// Offset 0), ranked by the water's footprint, whose slot the water samples in place of its probe term, writing a zero SSR
		// multiplier. A camera 1 m above calm, smooth water looks at a green emitting marker standing on it 6 m ahead, in a scene
		// with no light and no ambient and SSR off: without the reflection the water reflects black where the marker's mirror
		// image lies; with it, green.
		TEST_CASE("Water: a planar reflection on a water reflects the scene in its base plane" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 1.0f, 2.0f), glm::vec3(0.0f, 0.2f, -4.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				const glm::vec3 markerCentre(0.0f, 1.2f, -6.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), markerCentre), .Mesh = QuadMeshHandle, .Materials = { GreenMarkerMaterialHandle }, .Entity = UUID(1) });
				WaterItem water = MakeWater(RenderWaterShape::Rectangle, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.Size = glm::vec2(40.0f);
				water.Roughness = 0.02f;
				water.DetailNormalStrength = 0.0f;
				water.FoamAmount = 0.0f;
				snapshot.Waters.push_back(water);

				const std::vector<glm::dvec3> probed = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 0);
				snapshot.PlanarReflections.push_back(PlanarReflectionItem{ .World = glm::mat4(1.0f), .Normal = glm::vec3(0.0f, 1.0f, 0.0f), .Offset = 0.0f, .Entity = UUID(600) });
				const std::vector<glm::dvec3> mirrored = RenderSceneColor(device, fixture, snapshot);
				const RenderStats& stats = fixture.Renderer().GetRenderStats();
				CHECK(stats.WaterSurfaces == 1);
				CHECK(stats.PlanarReflections == 1);
				REQUIRE(stats.PlanarReflectionEntities.size() == 1);
				CHECK(stats.PlanarReflectionEntities[0] == UUID(600));

				const glm::vec2 image = ProjectToPixel(snapshot.Camera.Projection * snapshot.Camera.View, glm::vec3(markerCentre.x, -markerCentre.y, markerCentre.z));
				const auto x = static_cast<uint32_t>(image.x);
				const auto y = static_cast<uint32_t>(image.y);
				REQUIRE(x < Size);
				REQUIRE(y < Size);
				INFO("the reflection's pixel (" << x << ", " << y << "): green " << Texel(probed, x, y).g << " without the planar reflection, " << Texel(mirrored, x, y).g
												<< " with it");
				CHECK(Texel(probed, x, y).g < 0.02);
				CHECK(Texel(mirrored, x, y).g > 0.1);
				// SSR neither traces nor composites over a water that reflects its plane: a zero multiplier, the water's flag kept.
				const Image roughness = ReadTexture(device, fixture.Renderer().GetSceneRoughnessTexture());
				CHECK((WordTexel(roughness, x, y, 2, 0) & 0x00FFFFFFU) == 0);
				CHECK(((WordTexel(roughness, x, y, 2, 1) >> 8) & 0xFFU) == 0xFFU);
			}
			device.RunGarbageCollection();
		}

		// The Underwater pass (decision 3, W47.3). A camera 2 m under calm water looks along it at a red emitting wall 10 m
		// away: the pass records (its row, RenderStats::Underwater) and the wall is fogged toward the water's UnderwaterColor over
		// its UnderwaterVisibility along the path from the near plane, exp(-9.9 / 15) of the red left and the rest the
		// underwater colour. A camera 2 m above the water looking at the same wall records no Underwater pass and sees the wall
		// as the render without water does.
		TEST_CASE("Underwater: a camera under the surface sees the underwater view, and one above it does not" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const auto hasRow = [&fixture](std::string_view row)
				{
					return Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), row) != nullptr;
				};
				for (const float height : { -2.0f, 2.0f })
				{
					CAPTURE(height);
					RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, height, 0.0f), glm::vec3(0.0f, height, -10.0f),
						glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
					snapshot.Camera.ClearColor = glm::vec3(0.0f);
					snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(20.0f)),
						.Mesh = QuadMeshHandle,
						.Materials = { RedFloorMaterialHandle },
						.Entity = UUID(1) });
					const std::vector<glm::dvec3> dry = RenderSceneColor(device, fixture, snapshot);
					WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
					water.FoamAmount = 0.0f;
					snapshot.Waters.push_back(water);
					const std::vector<glm::dvec3> wet = RenderSceneColor(device, fixture, snapshot);
					const glm::dvec3 before = Texel(dry, 32, 32);
					const glm::dvec3 after = Texel(wet, 32, 32);
					INFO("the wall's centre: dry " << before.r << ", " << before.g << "; wet " << after.r << ", " << after.g << ", " << after.b);
					REQUIRE(before.r > 0.95);
					if (height < 0.0f)
					{
						CHECK(fixture.Renderer().GetRenderStats().Underwater);
						CHECK(hasRow("Underwater"));
						const double distance = 10.0 - static_cast<double>(snapshot.Camera.NearClip);
						const double kept = std::exp(-distance / static_cast<double>(water.UnderwaterVisibility));
						CHECK(std::abs(after.r - (before.r * kept + static_cast<double>(water.UnderwaterColor.r) * (1.0 - kept))) < 0.02);
						CHECK(std::abs(after.g - static_cast<double>(water.UnderwaterColor.g) * (1.0 - kept)) < 0.01);
						CHECK(std::abs(after.b - static_cast<double>(water.UnderwaterColor.b) * (1.0 - kept)) < 0.01);
					}
					else
					{
						CHECK_FALSE(fixture.Renderer().GetRenderStats().Underwater);
						CHECK_FALSE(hasRow("Underwater"));
						CHECK(after == before);
					}
				}
			}
			device.RunGarbageCollection();
		}

		// The waterline (decision 3, W47.3): a camera whose eye lies 2 cm above a swell's surface under it looks along the water
		// at a red emitting wall. The water's own colours are black, the environment and the lights too, so a pixel holds green
		// only where the Underwater pass fogged it toward the green UnderwaterColor (or a back face's total internal reflection
		// shows it, seen only from under the water). Each column of the image has its fogged pixels in one run from the bottom
		// up to one waterline, where the near plane crosses the surface: within two rows of the first whose point on the near
		// plane lies below the CPU surface.
		TEST_CASE("Underwater: a camera straddling the surface draws one waterline" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				const double time = 2.5;
				const WaterSurfaceDesc surface = ResolveWaterSurface(SwellWaves, 1.0, glm::dvec2(0.0), MakeWaterFrame(glm::dmat4(1.0)));
				const double under = EvaluateWaterSurface(surface, glm::dvec2(0.0, 0.0), time).Height;
				const auto eye = glm::vec3(0.0f, static_cast<float>(under + 0.02), 0.0f);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, eye, eye + glm::vec3(0.0f, 0.0f, -10.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				snapshot.Camera.ClearColor = glm::vec3(0.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(20.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { RedFloorMaterialHandle },
					.Entity = UUID(1) });
				WaterItem water = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), SwellWaves, time, 600, 0);
				water.ShallowColor = glm::vec3(0.0f);
				water.DeepColor = glm::vec3(0.0f);
				water.FoamAmount = 0.0f;
				snapshot.Waters.push_back(water);
				const std::vector<glm::dvec3> image = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().Underwater);

				// Each pixel's point on the near plane (reverse-Z depth 1) through the inverse view-projection, and the CPU surface
				// under it: the first row of a column whose point lies below it is where the waterline is expected.
				const glm::dmat4 inverse = glm::inverse(glm::dmat4(snapshot.Camera.Projection) * glm::dmat4(snapshot.Camera.View));
				const auto nearPoint = [&inverse](uint32_t x, uint32_t y)
				{
					const glm::dvec4 point = inverse
						* glm::dvec4((static_cast<double>(x) + 0.5) / Size * 2.0 - 1.0, 1.0 - (static_cast<double>(y) + 0.5) / Size * 2.0, 1.0, 1.0);
					return glm::dvec3(point) / point.w;
				};
				for (uint32_t x = 0; x < Size; x += 7)
				{
					uint32_t expectedRow = Size;
					for (uint32_t y = 0; y < Size && expectedRow == Size; ++y)
					{
						const glm::dvec3 point = nearPoint(x, y);
						if (point.y < EvaluateWaterSurface(surface, glm::dvec2(point.x, point.z), time).Height)
							expectedRow = y;
					}
					uint32_t transitions = 0;
					uint32_t firstFogged = Size;
					for (uint32_t y = 0; y < Size; ++y)
					{
						const bool fogged = Texel(image, x, y).g > 0.005;
						if (fogged && firstFogged == Size)
							firstFogged = y;
						if (y > 0 && fogged != (Texel(image, x, y - 1).g > 0.005))
							++transitions;
					}
					CAPTURE(x);
					CAPTURE(expectedRow);
					CAPTURE(firstFogged);
					REQUIRE(expectedRow > 8);
					REQUIRE(expectedRow < Size - 8);
					CHECK(transitions == 1);
					CHECK(Texel(image, x, Size - 1).g > 0.005);
					CHECK(std::abs(static_cast<double>(firstFogged) - static_cast<double>(expectedRow)) <= 2.0);
				}
			}
			device.RunGarbageCollection();
		}

		// W47.3's review, finding 1: an Ocean's planar reflection covers its base plane below the horizon
		// (ComputeWaterPlanarReflectionScreenRect), so a camera above the sea looking up at the sky, with no sea on screen,
		// renders no mirror view (the reflection counted culled), and one looking down at the sea renders it.
		TEST_CASE("Water: an Ocean's planar reflection renders no mirror view where no sea is on screen" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				for (const bool up : { true, false })
				{
					CAPTURE(up);
					RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 2.0f, 0.0f),
						up ? glm::vec3(0.0f, 3.0f, -1.0f) : glm::vec3(0.0f, 0.0f, -6.0f), glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
					snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 1.2f, -6.0f)), .Mesh = QuadMeshHandle, .Materials = { GreenMarkerMaterialHandle }, .Entity = UUID(1) });
					snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0));
					snapshot.PlanarReflections.push_back(PlanarReflectionItem{ .World = glm::mat4(1.0f), .Normal = glm::vec3(0.0f, 1.0f, 0.0f), .Offset = 0.0f, .Entity = UUID(600) });
					const Status rendered = RenderStatus(device, fixture, snapshot);
					REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
					const RenderStats& stats = fixture.Renderer().GetRenderStats();
					CHECK(stats.WaterSurfaces == 1);
					CHECK(stats.PlanarReflections == (up ? 0U : 1U));
					CHECK(stats.CulledPlanarReflections == (up ? 1U : 0U));
					CHECK((Test::SceneRendererIntegrationFixture::FindPass(stats, "PlanarReflection0") != nullptr) == !up);
				}
			}
			device.RunGarbageCollection();
		}

		// W47.3's review, finding 2: a water's planar reflection is its surface's alone; a mesh its entity also draws (a pool's
		// basin) is not the mirror's glass and keeps its own reflection. A camera 1 m above calm, smooth water looks at a green
		// emitting wall standing on it 10 m away, in a scene with no light and no ambient; the water entity also draws a polished
		// metal quad above the water. With the water's planar reflection the water shows the wall's mirror image, while the
		// quad's pixels stay exactly those of the render without it (it reflects the black environment), where sampling the
		// mirror would show the wall's green.
		TEST_CASE("Water: a water's planar reflection is its surface's, never its entity's meshes'" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, 1.0f, 2.0f), glm::vec3(0.0f, 0.2f, -4.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				const glm::mat4 wall = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 5.0f, -10.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(10.0f, 5.0f, 1.0f));
				snapshot.Meshes.push_back(MeshDrawItem{ .World = wall, .Mesh = QuadMeshHandle, .Materials = { GreenMarkerMaterialHandle }, .Entity = UUID(1) });
				const glm::vec3 quadCentre(-2.0f, 1.0f, -4.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), quadCentre) * glm::scale(glm::mat4(1.0f), glm::vec3(0.6f)),
					.Mesh = QuadMeshHandle,
					.Materials = { MirrorMaterialHandle },
					.Entity = UUID(600) });
				WaterItem water = MakeWater(RenderWaterShape::Rectangle, glm::mat4(1.0f), {}, 0.0, 600, 0);
				water.Size = glm::vec2(40.0f);
				water.Roughness = 0.02f;
				water.DetailNormalStrength = 0.0f;
				water.FoamAmount = 0.0f;
				snapshot.Waters.push_back(water);

				const std::vector<glm::dvec3> probed = RenderSceneColor(device, fixture, snapshot);
				snapshot.PlanarReflections.push_back(PlanarReflectionItem{ .World = glm::mat4(1.0f), .Normal = glm::vec3(0.0f, 1.0f, 0.0f), .Offset = 0.0f, .Entity = UUID(600) });
				const std::vector<glm::dvec3> mirrored = RenderSceneColor(device, fixture, snapshot);
				CHECK(fixture.Renderer().GetRenderStats().PlanarReflections == 1);

				const glm::mat4 viewProjection = snapshot.Camera.Projection * snapshot.Camera.View;
				// The water shows the wall's mirror image at the wall's point (0, 2, -10) mirrored in the plane.
				const glm::vec2 reflection = ProjectToPixel(viewProjection, glm::vec3(0.0f, -2.0f, -10.0f));
				REQUIRE(reflection.x < static_cast<float>(Size));
				REQUIRE(reflection.y < static_cast<float>(Size));
				const auto rx = static_cast<uint32_t>(reflection.x);
				const auto ry = static_cast<uint32_t>(reflection.y);
				INFO("the reflection's pixel (" << rx << ", " << ry << "): green " << Texel(probed, rx, ry).g << " without, " << Texel(mirrored, rx, ry).g << " with");
				CHECK(Texel(probed, rx, ry).g < 0.02);
				CHECK(Texel(mirrored, rx, ry).g > 0.1);
				// The quad's 3 x 3 pixels round its centre are those of the render without the planar reflection.
				const glm::vec2 quad = ProjectToPixel(viewProjection, quadCentre);
				const auto qx = static_cast<uint32_t>(quad.x);
				const auto qy = static_cast<uint32_t>(quad.y);
				REQUIRE(qx > 0);
				REQUIRE(qx + 1 < Size);
				REQUIRE(qy > 0);
				REQUIRE(qy + 1 < Size);
				for (uint32_t y = qy - 1; y <= qy + 1; ++y)
				{
					for (uint32_t x = qx - 1; x <= qx + 1; ++x)
					{
						CAPTURE(x);
						CAPTURE(y);
						CHECK(Texel(mirrored, x, y) == Texel(probed, x, y));
					}
				}
			}
			device.RunGarbageCollection();
		}

		// W47.3's review, finding 3 (a): the Underwater pass's Rectangle half, S05's grotto. An Ocean at y = 0 (its fog blue) and
		// a calm Rectangle lake at y = -6, 8 x 8 m, 3 m deep with 4 m of headroom (its fog red over 10 m), and a green emitting
		// wall inside the footprint, 6 m ahead of a camera at its centre looking along -Z:
		//   - at y = -4, in the lake's dry volume though 4 m under the sea's level, the pass records (the conservative CPU test
		//     ignores masks) but the wall's pixel is the dry render's: the lake masks the sea, and its air is dry;
		//   - at y = -7, under the lake, the wall is fogged toward the lake's red over the 5.9 m from the near plane, not the sea's
		//     blue;
		//   - with the wall moved 6 m beyond the lake's side, the fog stops at the side: exp(-6.9 / 10) of the green kept, not
		//     exp(-12.9 / 10).
		TEST_CASE("Underwater: a Rectangle's volume keeps its air dry, fogs its lake toward its own colour and ends the fog at its sides"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				WaterItem sea = MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0);
				sea.FoamAmount = 0.0f;
				sea.UnderwaterColor = glm::vec3(0.0f, 0.0f, 0.5f);
				WaterItem lake = MakeWater(RenderWaterShape::Rectangle, glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -6.0f, 0.0f)), {}, 0.0, 601, 0);
				lake.Size = glm::vec2(8.0f);
				lake.Depth = 3.0f;
				lake.Headroom = 4.0f;
				lake.FoamAmount = 0.0f;
				lake.UnderwaterColor = glm::vec3(0.5f, 0.0f, 0.0f);
				lake.UnderwaterVisibility = 10.0f;
				const auto render = [&fixture, &device, &sea, &lake](float height, float wallZ, bool wet)
				{
					RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, height, 3.0f), glm::vec3(0.0f, height, -10.0f),
						glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
					snapshot.Camera.ClearColor = glm::vec3(0.0f);
					snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, height, wallZ)) * glm::scale(glm::mat4(1.0f), glm::vec3(20.0f)),
						.Mesh = QuadMeshHandle,
						.Materials = { GreenMarkerMaterialHandle },
						.Entity = UUID(1) });
					if (wet)
					{
						snapshot.Waters.push_back(sea);
						snapshot.Waters.push_back(lake);
					}
					return Texel(RenderSceneColor(device, fixture, snapshot), 32, 32);
				};
				const double nearClip = static_cast<double>(fixture.Snapshot(RenderProjection::Perspective).Camera.NearClip);

				const glm::dvec3 dryAir = render(-4.0f, -3.0f, false);
				REQUIRE(dryAir.g > 1.9);
				const glm::dvec3 air = render(-4.0f, -3.0f, true);
				CHECK(fixture.Renderer().GetRenderStats().Underwater);
				CHECK(air == dryAir);

				const glm::dvec3 dryLake = render(-7.0f, -3.0f, false);
				const glm::dvec3 inLake = render(-7.0f, -3.0f, true);
				CHECK(fixture.Renderer().GetRenderStats().Underwater);
				const double keptInside = std::exp(-(6.0 - nearClip) / 10.0);
				INFO("in the lake: dry " << dryLake.g << ", wet " << inLake.r << ", " << inLake.g << ", " << inLake.b);
				CHECK(std::abs(inLake.g - dryLake.g * keptInside) < 0.03);
				CHECK(std::abs(inLake.r - 0.5 * (1.0 - keptInside)) < 0.02);
				CHECK(inLake.b < 0.01);

				const glm::dvec3 dryBeyond = render(-7.0f, -10.0f, false);
				const glm::dvec3 beyond = render(-7.0f, -10.0f, true);
				const double keptToSide = std::exp(-(7.0 - nearClip) / 10.0);
				INFO("beyond the side: dry " << dryBeyond.g << ", wet " << beyond.r << ", " << beyond.g << ", " << beyond.b);
				CHECK(std::abs(beyond.g - dryBeyond.g * keptToSide) < 0.03);
				CHECK(std::abs(beyond.r - 0.5 * (1.0 - keptToSide)) < 0.02);
				CHECK(beyond.b < 0.01);
			}
			device.RunGarbageCollection();
		}

		// W47.3's review, finding 3 (b): the Underwater pass's set (t0 the records, t5 SceneDepth) failing to be created leaves
		// the view unfogged with its surfaces drawn, and the render returns the error (RenderStats::Underwater false; the row,
		// begun before the set, stays with the time it took); the next render records the pass.
		TEST_CASE("Underwater: a pass whose set cannot be created leaves the view unfogged, returns the error, and the next render records it"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				PublishAssets(fixture);
				RenderSnapshot snapshot = MakeView(fixture, RenderProjection::Perspective, glm::vec3(0.0f, -2.0f, 0.0f), glm::vec3(0.0f, -2.0f, -10.0f),
					glm::vec3(0.0f, 1.0f, 0.0f), 100.0f);
				snapshot.Camera.ClearColor = glm::vec3(0.0f);
				snapshot.Meshes.push_back(MeshDrawItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(20.0f)),
					.Mesh = QuadMeshHandle,
					.Materials = { RedFloorMaterialHandle },
					.Entity = UUID(1) });
				const glm::dvec3 dry = Texel(RenderSceneColor(device, fixture, snapshot), 32, 32);
				snapshot.Waters.push_back(MakeWater(RenderWaterShape::Ocean, glm::mat4(1.0f), {}, 0.0, 600, 0));

				Status failed;
				{
					const Test::BindingSetCreationFails fails(device, [](const nvrhi::BindingSetDesc& desc, const nvrhi::IBindingLayout& /*layout*/)
					{
						return desc.bindings.size() == 2 && desc.bindings[0].type == nvrhi::ResourceType::StructuredBuffer_SRV
							&& desc.bindings[1].type == nvrhi::ResourceType::Texture_SRV;
					});
					failed = RenderStatus(device, fixture, snapshot);
					CHECK(fails.GetFailedCount() == 1);
				}
				CHECK_FALSE(failed.has_value());
				const RenderStats& unfogged = fixture.Renderer().GetRenderStats();
				CHECK_FALSE(unfogged.Underwater);
				CHECK(unfogged.WaterSurfaces == 1);
				CHECK(glm::dvec3(HalfTexel(ReadTexture(device, fixture.Renderer().GetSceneColorTexture()), 32, 32, 4)) == dry);

				const Status recovered = RenderStatus(device, fixture, snapshot);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK(fixture.Renderer().GetRenderStats().Underwater);
				CHECK(glm::dvec3(HalfTexel(ReadTexture(device, fixture.Renderer().GetSceneColorTexture()), 32, 32, 4)) != dry);
			}
			device.RunGarbageCollection();
		}
	}

}
