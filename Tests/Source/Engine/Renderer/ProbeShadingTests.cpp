#include "TestsPCH.h"

#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/HostImageUpload.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/BrdfLut.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/ProbeConstants.h"
#include "Support/DeviceLossTest.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <span>
#include <utility>
#include <vector>

// Probe shading (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B; Architecture §8.6): the
// forward passes' indirect term from the snapshot's irradiance volumes and reflection probes (Common/Probes.slang), driven by
// synthetic snapshots over in-memory probe data. The two oracles of the acceptance ("the furnace and colour-bleed oracles
// pass") are the GPU halves of ProbeCaptureTests.cpp's CPU projections: a volume whose probes hold a furnace's uniform
// irradiance gives a white surface the furnace's radiance, and a volume of reference SH9 probes shades a white floor as the
// CPU's trilinear evaluation of those probes predicts.

namespace Engine {

	namespace {

		constexpr AssetHandle FloorMaterialHandle{ 0x9a01 };
		constexpr AssetHandle EnvironmentHandle{ 0x9a02 };
		constexpr AssetHandle VolumeHandle{ 0x9a03 };
		constexpr AssetHandle ProbeEnvironmentHandle{ 0x9a04 };
		constexpr AssetHandle SecondVolumeHandle{ 0x9a05 };
		constexpr AssetHandle OtherProbeEnvironmentHandle{ 0x9a06 };
		constexpr uint32_t ViewSize = 64;
		// Y00, the constant SH basis function: a coefficient c00 = L / Y00 evaluates to L in every direction.
		constexpr double ShConstant = 0.28209479177387814;

		using ProbeSH9 = std::array<glm::vec3, 9>;

		// The SH9 (irradiance / pi, EnvironmentData.h's convention) of a furnace of uniform radiance `radiance`.
		ProbeSH9 MakeUniformSH9(const glm::vec3& radiance)
		{
			ProbeSH9 coefficients{};
			coefficients[0] = radiance / static_cast<float>(ShConstant);
			return coefficients;
		}

		// A volume of `counts` probes whose probe (x, y, z) holds `probe(x, y, z)`, baked for a box of `size`.
		AssetRef<Asset> MakeVolume(const glm::uvec3& counts, const glm::vec3& size, const std::function<ProbeSH9(uint32_t, uint32_t, uint32_t)>& probe)
		{
			IrradianceVolumeData volume;
			volume.CountX = counts.x;
			volume.CountY = counts.y;
			volume.CountZ = counts.z;
			volume.Size = size;
			for (uint32_t z = 0; z < counts.z; ++z)
			{
				for (uint32_t y = 0; y < counts.y; ++y)
				{
					for (uint32_t x = 0; x < counts.x; ++x)
						volume.Probes.push_back(IrradianceProbe{ .SH9 = probe(x, y, z) });
				}
			}
			return CreateRef<IrradianceVolumeData>(std::move(volume));
		}

		// An environment whose specular cube of `faceSize` and `mipCount` levels holds `radiance(direction)` (binary16) at every
		// texel of every level, with a black 1² skybox and the SH9 `sh9`.
		AssetRef<Asset> MakeEnvironment(uint32_t faceSize, uint32_t mipCount, const std::function<glm::dvec3(const glm::dvec3&)>& radiance, const ProbeSH9& sh9)
		{
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = 1, .MipCount = 1, .Texels = Buffer(ComputeCubeMapByteSize(1, 1), std::byte{ 0 }) };
			CubeMapData specular{ .FaceSize = faceSize, .MipCount = mipCount, .Texels = Buffer(ComputeCubeMapByteSize(faceSize, mipCount)) };
			size_t offset = 0;
			const uint16_t one = Test::DoubleToHalf(1.0);
			for (uint32_t level = 0; level < mipCount; ++level)
			{
				const uint32_t edge = std::max(1U, faceSize >> level);
				for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
				{
					for (uint32_t row = 0; row < edge; ++row)
					{
						for (uint32_t column = 0; column < edge; ++column)
						{
							const glm::dvec3 value = radiance(Test::CubeTexelDirection(face, column, row, edge));
							const std::array<uint16_t, 4> texel = { Test::DoubleToHalf(value.x), Test::DoubleToHalf(value.y), Test::DoubleToHalf(value.z), one };
							std::memcpy(specular.Texels.data() + offset, texel.data(), sizeof(texel));
							offset += sizeof(texel);
						}
					}
				}
			}
			environment.Specular = std::move(specular);
			environment.IrradianceSH9 = sh9;
			return CreateRef<EnvironmentData>(std::move(environment));
		}

		// A uniform environment of `radiance` in its specular cube, with the SH9 `sh9`.
		AssetRef<Asset> MakeUniformEnvironment(const glm::dvec3& radiance, const ProbeSH9& sh9)
		{
			return MakeEnvironment(8, 4, [radiance](const glm::dvec3&)
			{
				return radiance;
			}, sh9);
		}

		// The floor: the fixture's 2 x 2 quad (facing +Z) turned to face +Y exactly and scaled to `halfExtent` metres about
		// (0, `height`, 0): local (x, y, 0) lands at (halfExtent x, height, -halfExtent y).
		glm::mat4 MakeFloorWorld(float height, float halfExtent)
		{
			return glm::mat4(glm::vec4(halfExtent, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, -halfExtent, 0.0f), glm::vec4(0.0f, halfExtent, 0.0f, 0.0f),
				glm::vec4(0.0f, height, 0.0f, 1.0f));
		}

		// The fixture's snapshot looking along `direction` at the origin through an orthographic camera `halfHeight` metres high,
		// its mesh the floor in `material` (white unless set), no light, no Environment map and a black ambient.
		RenderSnapshot MakeFloorSnapshot(Test::SceneRendererIntegrationFixture& fixture, const glm::vec3& direction, float halfHeight, float floorHeight,
			float floorHalfExtent)
		{
			RenderSnapshot snapshot = fixture.Snapshot(RenderProjection::Orthographic);
			const glm::vec3 forward = glm::normalize(direction);
			snapshot.Camera.Position = -forward * 10.0f;
			snapshot.Camera.View = glm::lookAt(snapshot.Camera.Position, glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f));
			snapshot.Camera.OrthographicSize = halfHeight;
			snapshot.Camera.NearClip = 0.1f;
			snapshot.Camera.FarClip = 30.0f;
			snapshot.Camera.Projection =
				ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, halfHeight, 0.1f, 30.0f, snapshot.Camera.ViewportWidth, snapshot.Camera.ViewportHeight);
			snapshot.Meshes.front().World = MakeFloorWorld(floorHeight, floorHalfExtent);
			snapshot.Meshes.front().Materials = { FloorMaterialHandle };
			snapshot.Meshes.front().CastShadows = false;
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
			snapshot.Post.ExposureEV = 0.0f;
			return snapshot;
		}

		// A white material: a dielectric (metallic 0) of perceptual roughness 1, or a metal of roughness 0.
		AssetRef<Asset> MakeWhiteMaterial(bool metal)
		{
			MaterialData material;
			material.Metallic = metal ? 1.0f : 0.0f;
			material.Roughness = metal ? 0.0f : 1.0f;
			return CreateRef<MaterialData>(std::move(material));
		}

		// The point of the floor plane y = `height` that pixel (x, y) of the snapshot's camera sees.
		glm::dvec3 GetFloorPoint(const CameraData& camera, uint32_t x, uint32_t y, float height)
		{
			const Result<RenderRay> ray = ComputeViewPixelRay(camera, x, y);
			REQUIRE_MESSAGE(ray.has_value(), ray.error().ToString());
			const glm::dvec3 origin(ray->Origin);
			const glm::dvec3 direction(ray->Direction);
			return origin + direction * ((static_cast<double>(height) - origin.y) / direction.y);
		}

		// The 8-bit sRGB value the Linear tonemap writes for the linear `value`.
		int ToSrgb8(double value)
		{
			return static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(std::clamp(value, 0.0, 1.0))));
		}

		// The specular albedo a white dielectric (f0 0.04) of perceptual roughness 1 has seen head-on: the DFG LUT's last
		// column and row, which the shader's clamped lookup reads (SceneRendererTests' grey-surface case), with the
		// multi-scatter compensation. Its diffuse term is the irradiance times 1 minus this.
		double GetHeadOnDielectricSpecularAlbedo()
		{
			constexpr double LutSize = BrdfLut::Size;
			const glm::dvec2 dfg = Test::ComputeDfg((LutSize - 0.5) / LutSize, (LutSize - 0.5) / LutSize, BrdfLut::LutSampleCount);
			constexpr double F0 = 0.04;
			return (dfg.x + (dfg.y - dfg.x) * F0) * (1.0 + F0 * (1.0 / dfg.y - 1.0));
		}

		glm::ivec3 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			return { Test::SceneRendererIntegrationFixture::Channel(image, x, y, 0), Test::SceneRendererIntegrationFixture::Channel(image, x, y, 1),
				Test::SceneRendererIntegrationFixture::Channel(image, x, y, 2) };
		}

		// Whether every channel of `actual` is within `tolerance` of `expected`.
		bool IsNear(const glm::ivec3& actual, const glm::ivec3& expected, int tolerance)
		{
			return std::abs(actual.x - expected.x) <= tolerance && std::abs(actual.y - expected.y) <= tolerance && std::abs(actual.z - expected.z) <= tolerance;
		}

		// The SH9 of irradiance / pi of a probe of the colour-bleed room (the reference projection of ProbeCaptureTests.cpp's
		// CPU half, Test::ProjectIrradianceSH9): a red wall fills its +X hemisphere, brighter the nearer the probe stands to it
		// (`x` of 2), and a grey room the rest, brighter higher up and further along Z.
		ProbeSH9 MakeColourBleedProbe(uint32_t x, uint32_t y, uint32_t z)
		{
			constexpr uint32_t CubeSize = 8;
			Test::ReferenceCube cube;
			cube.Size = CubeSize;
			const double red = 0.25 + 0.375 * x;
			const double grey = 0.2 + 0.2 * y + 0.1 * z;
			for (uint32_t face = 0; face < CubeMapData::FaceCount; ++face)
			{
				for (uint32_t row = 0; row < CubeSize; ++row)
				{
					for (uint32_t column = 0; column < CubeSize; ++column)
					{
						const glm::dvec3 direction = Test::CubeTexelDirection(face, column, row, CubeSize);
						cube.Faces[face].push_back(direction.x > 0.0 ? glm::dvec3(red, 0.0, 0.0) : glm::dvec3(grey));
					}
				}
			}
			const std::array<glm::dvec3, 9> projected = Test::ProjectIrradianceSH9(cube);
			ProbeSH9 coefficients{};
			for (size_t index = 0; index < coefficients.size(); ++index)
				coefficients[index] = glm::vec3(projected[index]);
			return coefficients;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SceneRenderer: an irradiance volume baked in a furnace lights a white surface with the furnace's radiance"
			* doctest::test_suite(Test::GpuSuite))
		{
			// A white Lambertian (dielectric, roughness 1) floor seen head-on reflects a furnace's radiance L: its diffuse term
			// L (1 - ms) and its specular term L ms make L. The scene's Environment holds the furnace's specular radiance but no
			// irradiance (its SH9 are zero), so without the volume the floor only reflects L ms; the volume's probes hold the
			// furnace's uniform irradiance, which replaces the Environment's inside its box. A bake's captures
			// (RenderQualitySettings::Probes false) add nothing.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			constexpr double Radiance = 0.5;
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(EnvironmentHandle, MakeUniformEnvironment(glm::dvec3(Radiance), ProbeSH9{}));
			fixture.Assets().Publish(VolumeHandle, MakeVolume(glm::uvec3(2, 3, 2), glm::vec3(8.0f, 4.0f, 8.0f), [](uint32_t, uint32_t, uint32_t)
			{
				return MakeUniformSH9(glm::vec3(static_cast<float>(Radiance)));
			}));

			RenderSnapshot unlit = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f, -0.5f, 3.0f);
			unlit.Environment.Environment = EnvironmentHandle;
			const Image withoutVolume = fixture.Render(unlit);
			RenderSnapshot snapshot = unlit;
			// The box reaches 1.5 m below the floor, beyond its blend distance: the volume alone lights the floor.
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(8.0f, 4.0f, 8.0f), .Data = VolumeHandle, .BlendDistance = 0.25f, .Entity = UUID(31) });
			const Image lit = fixture.Render(snapshot);

			const int expected = ToSrgb8(Radiance);
			const int withoutExpected = ToSrgb8(Radiance * GetHeadOnDielectricSpecularAlbedo());
			CAPTURE(expected);
			CAPTURE(withoutExpected);
			for (uint32_t y = 0; y < ViewSize; ++y)
			{
				for (uint32_t x = 0; x < ViewSize; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					REQUIRE(IsNear(GetPixel(lit, x, y), glm::ivec3(expected), 1));
					REQUIRE(IsNear(GetPixel(withoutVolume, x, y), glm::ivec3(withoutExpected), 1));
				}
			}
			CHECK(fixture.Cache().GetStats().IrradianceVolumeCount == 1);

			snapshot.Quality.Probes = false;
			const Image capture = fixture.Render(snapshot);
			CHECK(capture.Pixels == withoutVolume.Pixels);
		}

		TEST_CASE("SceneRenderer: a reflection probe baked in a furnace reflects the furnace's radiance" * doctest::test_suite(Test::GpuSuite))
		{
			// The furnace's other half: the scene's Environment holds the furnace's irradiance but no specular radiance, so the
			// white floor reflects only L (1 - ms) without the probe; the reflection probe's cube holds the furnace's radiance,
			// which replaces the Environment's specular inside its box, so the floor reflects L.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			constexpr double Radiance = 0.5;
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(EnvironmentHandle, MakeUniformEnvironment(glm::dvec3(0.0), MakeUniformSH9(glm::vec3(static_cast<float>(Radiance)))));
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeUniformEnvironment(glm::dvec3(Radiance), ProbeSH9{}));

			RenderSnapshot unlit = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f, -0.5f, 3.0f);
			unlit.Environment.Environment = EnvironmentHandle;
			const Image withoutProbe = fixture.Render(unlit);
			RenderSnapshot snapshot = unlit;
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(8.0f, 4.0f, 8.0f), .Data = ProbeEnvironmentHandle, .BlendDistance = 0.25f, .Entity = UUID(32) });
			const Image lit = fixture.Render(snapshot);

			const int expected = ToSrgb8(Radiance);
			const int withoutExpected = ToSrgb8(Radiance * (1.0 - GetHeadOnDielectricSpecularAlbedo()));
			CAPTURE(expected);
			CAPTURE(withoutExpected);
			for (uint32_t y = 0; y < ViewSize; ++y)
			{
				for (uint32_t x = 0; x < ViewSize; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					REQUIRE(IsNear(GetPixel(lit, x, y), glm::ivec3(expected), 1));
					REQUIRE(IsNear(GetPixel(withoutProbe, x, y), glm::ivec3(withoutExpected), 1));
				}
			}
		}

		TEST_CASE("SceneRenderer: a volume beside a red wall tints a white floor as the reference irradiance predicts" * doctest::test_suite(Test::GpuSuite))
		{
			// A 3 x 2 x 3 volume over a 4 x 2 x 4 box whose probes see a red wall in their +X hemisphere, the nearer the wall the
			// redder, and a grey room elsewhere (each the reference SH9 projection of its surroundings). A white floor half a metre
			// below the box's centre, seen straight down, between the probes' layers and columns, reflects the irradiance the
			// eight probes around each point give, trilinear in the grid and evaluated at the floor's normal on the CPU, times
			// its diffuse share 1 - ms (no other light: the ambient is black and nothing reflects).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			const glm::uvec3 counts(3, 2, 3);
			const glm::vec3 boxSize(4.0f, 2.0f, 4.0f);
			constexpr float FloorHeight = -0.5f;
			std::vector<ProbeSH9> probes;
			for (uint32_t z = 0; z < counts.z; ++z)
			{
				for (uint32_t y = 0; y < counts.y; ++y)
				{
					for (uint32_t x = 0; x < counts.x; ++x)
						probes.push_back(MakeColourBleedProbe(x, y, z));
				}
			}
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(VolumeHandle, MakeVolume(counts, boxSize, [&probes, counts](uint32_t x, uint32_t y, uint32_t z)
			{
				return probes[x + counts.x * (y + counts.y * z)];
			}));

			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 1.9f, FloorHeight, 3.0f);
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = boxSize, .Data = VolumeHandle, .BlendDistance = 0.0f, .Entity = UUID(33) });
			const Image image = fixture.Render(snapshot);

			const double diffuseShare = 1.0 - GetHeadOnDielectricSpecularAlbedo();
			const glm::dvec3 normal(0.0, 1.0, 0.0);
			bool tinted = false;
			for (uint32_t y = 2; y < ViewSize; y += 6)
			{
				for (uint32_t x = 2; x < ViewSize; x += 6)
				{
					// The probes around the floor point, trilinear in the grid (IrradianceVolumeData.h's placement).
					const glm::dvec3 point = GetFloorPoint(snapshot.Camera, x, y, FloorHeight);
					const glm::dvec3 cells = glm::dvec3(counts) - 1.0;
					const glm::dvec3 grid = glm::clamp((point / glm::dvec3(boxSize) + 0.5) * cells, glm::dvec3(0.0), cells);
					const glm::uvec3 base = glm::min(glm::uvec3(grid), counts - 2U);
					const glm::dvec3 fraction = grid - glm::dvec3(base);
					std::array<glm::dvec3, 9> coefficients{};
					for (uint32_t corner = 0; corner < 8; ++corner)
					{
						const glm::uvec3 step(corner & 1U, (corner >> 1) & 1U, corner >> 2);
						const glm::uvec3 probe = base + step;
						double weight = 1.0;
						for (int axis = 0; axis < 3; ++axis)
							weight *= step[axis] != 0 ? fraction[axis] : 1.0 - fraction[axis];
						const ProbeSH9& stored = probes[probe.x + counts.x * (probe.y + counts.y * probe.z)];
						for (size_t index = 0; index < coefficients.size(); ++index)
							coefficients[index] += glm::dvec3(stored[index]) * weight;
					}
					const glm::dvec3 radiance = glm::max(Test::EvaluateIrradianceSH9(coefficients, normal), glm::dvec3(0.0)) * diffuseShare;
					const glm::ivec3 expected(ToSrgb8(radiance.x), ToSrgb8(radiance.y), ToSrgb8(radiance.z));
					CAPTURE(x);
					CAPTURE(y);
					CAPTURE(expected);
					CHECK(IsNear(GetPixel(image, x, y), expected, 2));
					tinted = tinted || expected.x > expected.y + 10;
				}
			}
			// The wall's red reaches the floor: the reference predicts a tint the image shows.
			CHECK(tinted);
		}

		TEST_CASE("SceneRenderer: a box-projected reflection probe reflects the face its ray reaches, seen from the probe" * doctest::test_suite(Test::GpuSuite))
		{
			// A white mirror-like metal floor at y = -0.9 inside a 10 x 2 x 10 probe whose cube is red towards +X and green towards
			// -X, seen by an orthographic camera looking down and towards -X: every pixel reflects the direction (-0.45, 0.89, 0),
			// green. Box-projected, the ray from a floor point (x, z) leaves through the ceiling at x - 0.95, which the probe at
			// the origin sees red where that is positive. Outside the box the floor reflects the black ambient.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			constexpr float FloorHeight = -0.9f;
			const glm::vec3 boxSize(10.0f, 2.0f, 10.0f);
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(true));
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeEnvironment(32, 6, [](const glm::dvec3& direction)
			{
				return direction.x > 0.0 ? glm::dvec3(1.0, 0.0, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
			}, ProbeSH9{}));
			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(-0.5f, -1.0f, 0.0f), 4.0f, FloorHeight, 8.0f);
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = boxSize, .Data = ProbeEnvironmentHandle, .BoxProjection = true, .BlendDistance = 0.0f, .Entity = UUID(34) });
			const Image projected = fixture.Render(snapshot);
			snapshot.ReflectionProbes.front().BoxProjection = false;
			const Image plain = fixture.Render(snapshot);

			const glm::ivec3 redColor(255, 0, 0);
			const glm::ivec3 greenColor(0, 255, 0);
			const glm::ivec3 blackColor(0, 0, 0);
			uint32_t redPixels = 0;
			uint32_t greenPixels = 0;
			for (uint32_t y = 0; y < ViewSize; ++y)
			{
				for (uint32_t x = 0; x < ViewSize; ++x)
				{
					const glm::dvec3 point = GetFloorPoint(snapshot.Camera, x, y, FloorHeight);
					// Pixels near the box's faces or the projected boundary at x = 0.95 are left out (filtering).
					const double margin = std::min(5.0 - std::abs(point.x), 5.0 - std::abs(point.z));
					if (std::abs(margin) < 0.3 || (margin > 0.0 && std::abs(point.x - 0.95) < 0.5))
						continue;
					CAPTURE(x);
					CAPTURE(y);
					CAPTURE(point.x);
					CAPTURE(point.z);
					if (margin < 0.0)
					{
						CHECK(IsNear(GetPixel(projected, x, y), blackColor, 2));
						CHECK(IsNear(GetPixel(plain, x, y), blackColor, 2));
						continue;
					}
					const bool red = point.x > 0.95;
					CHECK(IsNear(GetPixel(projected, x, y), red ? redColor : greenColor, 12));
					CHECK(IsNear(GetPixel(plain, x, y), greenColor, 12));
					redPixels += red ? 1 : 0;
					greenPixels += red ? 0 : 1;
				}
			}
			CHECK(redPixels > 100);
			CHECK(greenPixels > 100);
		}

		TEST_CASE("SceneRenderer: the smaller of overlapping volumes wins, fading into the larger over its blend distance, in any order"
			* doctest::test_suite(Test::GpuSuite))
		{
			// A large blue volume (8 x 4 x 8, no blend distance) and a small red one (2 x 4 x 2, blend distance 0.5 m) over a
			// white floor seen straight down: the floor is red inside the small box farther than 0.5 m from its sides, blue outside
			// it, a blend between, and black outside both, whichever volume the snapshot lists first.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			constexpr float FloorHeight = -0.5f;
			const glm::dvec3 blue(0.0, 0.0, 0.5);
			const glm::dvec3 red(0.5, 0.0, 0.0);
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(VolumeHandle, MakeVolume(glm::uvec3(2), glm::vec3(8.0f, 4.0f, 8.0f), [&blue](uint32_t, uint32_t, uint32_t)
			{
				return MakeUniformSH9(glm::vec3(blue));
			}));
			fixture.Assets().Publish(SecondVolumeHandle, MakeVolume(glm::uvec3(2), glm::vec3(2.0f, 4.0f, 2.0f), [&red](uint32_t, uint32_t, uint32_t)
			{
				return MakeUniformSH9(glm::vec3(red));
			}));
			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 5.0f, FloorHeight, 6.0f);
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(2.0f, 4.0f, 2.0f), .Data = SecondVolumeHandle, .BlendDistance = 0.5f, .Entity = UUID(35) });
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(8.0f, 4.0f, 8.0f), .Data = VolumeHandle, .BlendDistance = 0.0f, .Entity = UUID(36) });
			const Image smallFirst = fixture.Render(snapshot);
			std::ranges::reverse(snapshot.IrradianceVolumes);
			const Image largeFirst = fixture.Render(snapshot);
			CHECK(smallFirst.Pixels == largeFirst.Pixels);

			const double diffuseShare = 1.0 - GetHeadOnDielectricSpecularAlbedo();
			uint32_t blended = 0;
			for (uint32_t y = 1; y < ViewSize; y += 2)
			{
				for (uint32_t x = 1; x < ViewSize; x += 2)
				{
					const glm::dvec3 point = GetFloorPoint(snapshot.Camera, x, y, FloorHeight);
					const double outsideLarge = std::max(std::abs(point.x), std::abs(point.z)) - 4.0;
					if (std::abs(outsideLarge) < 0.1)
						continue;
					const double insideSmall = std::min(1.0 - std::abs(point.x), 1.0 - std::abs(point.z));
					const double weight = std::clamp(insideSmall / 0.5, 0.0, 1.0);
					const glm::dvec3 radiance = outsideLarge > 0.0 ? glm::dvec3(0.0) : (blue * (1.0 - weight) + red * weight) * diffuseShare;
					const glm::ivec3 expected(ToSrgb8(radiance.x), ToSrgb8(radiance.y), ToSrgb8(radiance.z));
					CAPTURE(x);
					CAPTURE(y);
					CAPTURE(expected);
					CHECK(IsNear(GetPixel(smallFirst, x, y), expected, 2));
					blended += weight > 0.1 && weight < 0.9 ? 1 : 0;
				}
			}
			CHECK(blended > 10);
		}

		TEST_CASE("SceneRenderer: probes beyond the view's limits and reflection cubes unlike the first are dropped with one warning each"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Nine volumes over the floor, each smaller than the one before and each lighting it with its own colour: the view
			// shades the first eight of the snapshot (the eighth, the smallest of them, lights the floor) and drops the ninth.
			// A second reflection probe whose cube is of another size is dropped too (one cube array holds the view's cubes),
			// so the first lights alone. Each drop is logged once per renderer.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			constexpr float FloorHeight = -0.5f;
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 1.0f, FloorHeight, 3.0f);
			for (uint32_t index = 0; index <= MaxViewIrradianceVolumes; ++index)
			{
				const AssetHandle handle(0x9b00 + index);
				const glm::vec3 radiance(0.05f * static_cast<float>(index + 1), 0.0f, 0.0f);
				fixture.Assets().Publish(handle, MakeVolume(glm::uvec3(2), glm::vec3(1.0f), [radiance](uint32_t, uint32_t, uint32_t)
				{
					return MakeUniformSH9(radiance);
				}));
				const float side = 20.0f - static_cast<float>(index);
				snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(side), .Data = handle, .BlendDistance = 0.0f, .Entity = UUID(100 + index) });
			}
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeUniformEnvironment(glm::dvec3(0.0, 0.25, 0.0), ProbeSH9{}));
			fixture.Assets().Publish(OtherProbeEnvironmentHandle, MakeEnvironment(16, 5, [](const glm::dvec3&)
			{
				return glm::dvec3(0.0, 0.0, 1.0);
			}, ProbeSH9{}));
			// The smaller second probe would win where both apply, were it kept.
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(10.0f), .Data = ProbeEnvironmentHandle, .BlendDistance = 0.0f, .Entity = UUID(120) });
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(8.0f), .Data = OtherProbeEnvironmentHandle, .BlendDistance = 0.0f, .Entity = UUID(121) });

			Image image;
			{
				Test::ExpectLog limit(LogLevel::Warn, "beyond the view's");
				Test::ExpectLog format(LogLevel::Warn, "differs in format or size");
				image = fixture.Render(snapshot);
				image = fixture.Render(snapshot);
				CHECK(limit.GetMatchCount() == 1);
				CHECK(format.GetMatchCount() == 1);
			}
			const double share = 1.0 - GetHeadOnDielectricSpecularAlbedo();
			const double specular = GetHeadOnDielectricSpecularAlbedo();
			const glm::ivec3 expected(ToSrgb8(0.05 * MaxViewIrradianceVolumes * share), ToSrgb8(0.25 * specular), 0);
			CAPTURE(expected);
			CHECK(IsNear(GetPixel(image, ViewSize / 2, ViewSize / 2), expected, 2));
		}

		// W26.1's review: the view's copies of its probes' data are its own (place 2) and it keeps no reference to the cache's
		// mirrors (place 3), so the cache stays their one owner: once their assets are gone, the out-of-memory trim
		// (GpuResourceCache::TrimUnused) frees the mirrors of the volume and the reflection probe the view drew last.
		TEST_CASE("SceneRenderer: the cache's trim frees the probe data a view drew" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(VolumeHandle, MakeVolume(glm::uvec3(2), glm::vec3(8.0f, 4.0f, 8.0f), [](uint32_t, uint32_t, uint32_t)
			{
				return MakeUniformSH9(glm::vec3(0.5f));
			}));
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeUniformEnvironment(glm::dvec3(0.5), ProbeSH9{}));
			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(0.0f, -1.0f, 0.0f), 2.0f, -0.5f, 3.0f);
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(8.0f, 4.0f, 8.0f), .Data = VolumeHandle, .BlendDistance = 0.25f, .Entity = UUID(39) });
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(8.0f, 4.0f, 8.0f), .Data = ProbeEnvironmentHandle, .BlendDistance = 0.25f, .Entity = UUID(40) });
			const Image lit = fixture.Render(snapshot);
			// The probes lit the floor, so the view copied both mirrors.
			CHECK(GetPixel(lit, ViewSize / 2, ViewSize / 2).x > ToSrgb8(0.25));
			const GpuIrradianceVolume* volume = fixture.Cache().GetIrradianceVolume(VolumeHandle);
			const GpuEnvironment* environment = fixture.Cache().GetEnvironment(ProbeEnvironmentHandle);
			REQUIRE(volume != nullptr);
			REQUIRE(environment != nullptr);
			const nvrhi::BufferHandle probes = volume->Probes;
			const nvrhi::TextureHandle cube = environment->Specular;
			REQUIRE(probes != nullptr);
			REQUIRE(cube != nullptr);

			// The assets are gone (their scene unloaded) and the device ran short of memory: the trim releases both mirrors, and
			// once the device has retired the render's command lists, this case's handles are their last holders, besides the
			// host-copy upload's own handle of a cube that took that path (HostImageUpload keeps it until nobody else holds the
			// texture; API 1.4 only).
			fixture.Assets().Remove(VolumeHandle);
			fixture.Assets().Remove(ProbeEnvironmentHandle);
			fixture.Cache().TrimUnused();
			CHECK(fixture.Cache().GetStats().IrradianceVolumeCount == 0);
			GraphicsDevice& device = gpu.GetDevice();
			device.WaitForIdle();
			device.RunGarbageCollection();
			const uint32_t cubeHolders = device.GetHostImageUpload().SupportsHostImageCopy(cube->getDesc()) ? 2 : 1;
			CHECK_FALSE(device.GetResourceTracker().HasOtherReferences(*probes, 1));
			CHECK_FALSE(device.GetResourceTracker().HasOtherReferences(*cube, cubeHolders));
		}

		TEST_CASE("SceneRenderer: a renderer lit by probes recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			// The probes' GPU objects are the cache's mirrors (place 3: the volume's coefficients, the probe's Environment) and
			// the view's copies (place 2: the constants, the coefficient buffer and the cube array): a loss drops them all, and
			// the first render after the recovery uploads and copies them again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			fixture.Assets().Publish(FloorMaterialHandle, MakeWhiteMaterial(false));
			fixture.Assets().Publish(VolumeHandle, MakeVolume(glm::uvec3(2, 2, 3), glm::vec3(6.0f, 2.0f, 6.0f), [](uint32_t x, uint32_t y, uint32_t z)
			{
				return MakeUniformSH9(glm::vec3(0.1f + 0.2f * static_cast<float>(x), 0.1f + 0.2f * static_cast<float>(y), 0.1f + 0.1f * static_cast<float>(z)));
			}));
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeEnvironment(16, 5, [](const glm::dvec3& direction)
			{
				return glm::dvec3(0.5 + 0.5 * direction.x, 0.5, 0.5 - 0.5 * direction.z);
			}, ProbeSH9{}));
			RenderSnapshot snapshot = MakeFloorSnapshot(fixture, glm::vec3(-0.3f, -1.0f, 0.2f), 2.5f, -0.5f, 4.0f);
			snapshot.IrradianceVolumes.push_back(IrradianceVolumeItem{ .Size = glm::vec3(6.0f, 2.0f, 6.0f), .Data = VolumeHandle, .BlendDistance = 0.5f, .Entity = UUID(37) });
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(6.0f, 2.0f, 6.0f), .Data = ProbeEnvironmentHandle, .BlendDistance = 0.5f, .Entity = UUID(38) });
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 70;
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
			// The probes lit the floor: its centre is not black.
			REQUIRE(report->Before.size() == static_cast<size_t>(ViewSize) * ViewSize * 4);
			const size_t centre = (static_cast<size_t>(ViewSize / 2) * ViewSize + ViewSize / 2) * 4;
			CHECK(std::to_integer<int>(report->Before[centre]) + std::to_integer<int>(report->Before[centre + 1]) > 40);
		}
	}

}
