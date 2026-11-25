#include "TestsPCH.h"

#include "Engine/Renderer/Fog.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SkyAtmosphere.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
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
#include <limits>
#include <optional>
#include <vector>

// The fog (M32, Docs/Decisions/0047-m32-fog-ssr.md decisions 3 to 6; Renderer/Fog.h): MakeFogConstants on the CPU, then the
// forward passes' fog over synthetic snapshots compared with the double-precision oracle (Test::EvaluateFog, which integrates
// the fog numerically where the shaders use the closed form). The fixture's quad (2 x 2 m, facing +Z, 3 m in front of the
// camera) gets a black, rough, emitting material and the scene no ambient light, so its radiance is its emission and the
// fogged radiance is emission (1 - a) + F a exactly; the cases read SceneColor (RGBA16_FLOAT) back.

namespace Engine {

	namespace {

		constexpr AssetHandle EmissiveMaterialHandle{ 0x9f01 };
		constexpr AssetHandle TransparentMaterialHandle{ 0x9f02 };
		constexpr AssetHandle SkyEnvironmentHandle{ 0x9f03 };
		constexpr glm::vec3 Emission(0.8f, 0.3f, 0.1f);
		constexpr glm::dvec3 SkyRadiance(0.1, 0.3, 0.9);

		AssetRef<Asset> MakeMaterial(const glm::vec4& baseColor, const glm::vec3& emissive, AlphaMode alphaMode)
		{
			MaterialData material;
			material.BaseColor = baseColor;
			material.Metallic = 0.0f;
			material.Roughness = 1.0f;
			material.Emissive = emissive;
			material.AlphaMode = alphaMode;
			return CreateRef<MaterialData>(std::move(material));
		}

		// An environment whose skybox holds `radiance` at its one texel (binary16), with a black prefiltered cube and no
		// irradiance.
		AssetRef<Asset> MakeSkyEnvironment(const glm::dvec3& radiance)
		{
			constexpr uint32_t FaceSize = 8;
			constexpr uint32_t MipCount = 4;
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = 1, .MipCount = 1, .Texels = Buffer(ComputeCubeMapByteSize(1, 1)) };
			const std::array<uint16_t, 4> texel = { Test::DoubleToHalf(radiance.x), Test::DoubleToHalf(radiance.y), Test::DoubleToHalf(radiance.z),
				Test::DoubleToHalf(1.0) };
			for (size_t offset = 0; offset < environment.Skybox.Texels.size(); offset += sizeof(texel))
				std::memcpy(environment.Skybox.Texels.data() + offset, texel.data(), sizeof(texel));
			environment.Specular = { .FaceSize = FaceSize, .MipCount = MipCount, .Texels = Buffer(ComputeCubeMapByteSize(FaceSize, MipCount), std::byte{ 0 }) };
			return CreateRef<EnvironmentData>(std::move(environment));
		}

		void PublishFogAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			fixture.Assets().Publish(EmissiveMaterialHandle, MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), Emission, AlphaMode::Opaque));
			fixture.Assets().Publish(TransparentMaterialHandle, MakeMaterial(glm::vec4(0.0f, 0.0f, 0.0f, 0.5f), glm::vec3(0.1f, 0.6f, 0.2f), AlphaMode::Blend));
			fixture.Assets().Publish(SkyEnvironmentHandle, MakeSkyEnvironment(SkyRadiance));
		}

		// A fog with distance and height fog, an ambient tint and a sun glow; the glow's light shines toward the camera, from
		// behind the quad, so it never lights the quad's front face.
		RenderFog MakeTestFog()
		{
			return RenderFog{ .Enabled = true,
				.Color = glm::vec3(0.3f, 0.5f, 0.7f),
				.AmbientTint = 0.0f,
				.Density = 0.12f,
				.StartDistance = 1.0f,
				.MaxOpacity = 0.9f,
				.HeightDensity = 0.4f,
				.HeightFalloff = 1.5f,
				.BaseHeight = -0.5f,
				.SunGlow = 0.5f,
				.SunGlowExponent = 6.0f,
				.FogSkybox = true };
		}

		// The fixture's snapshot with the quad (or a lifted copy) emitting through the emissive material, no ambient light, the
		// Linear tonemapper and, when `fog` is set, that fog with its sun.
		RenderSnapshot MakeFogSnapshot(Test::SceneRendererIntegrationFixture& fixture, RenderProjection projection, const std::optional<RenderFog>& fog)
		{
			RenderSnapshot snapshot = fixture.Snapshot(projection);
			snapshot.Meshes.front().Materials = { EmissiveMaterialHandle };
			snapshot.Meshes.front().CastShadows = false;
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
			if (fog)
			{
				snapshot.Fog = *fog;
				snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
					.Color = glm::vec3(1.0f, 0.9f, 0.6f),
					.Intensity = 2.0f,
					.Direction = glm::normalize(glm::vec3(0.2f, 0.1f, 1.0f)) });
			}
			return snapshot;
		}

		// SceneColor of the last render, as linear RGBA.
		std::vector<glm::dvec4> ReadSceneColor(GraphicsDevice& device, SceneRenderer& renderer)
		{
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*renderer.GetSceneColorTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			std::vector<glm::dvec4> texels;
			for (uint32_t y = 0; y < image->Height; ++y)
			{
				const std::span<const std::byte> row = image->GetRow(y);
				for (uint32_t x = 0; x < image->Width; ++x)
				{
					std::array<uint16_t, 4> halves{};
					std::memcpy(halves.data(), row.data() + static_cast<size_t>(x) * sizeof(halves), sizeof(halves));
					texels.emplace_back(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]), Test::HalfToDouble(halves[2]), Test::HalfToDouble(halves[3]));
				}
			}
			return texels;
		}

		// Where the camera ray of pixel (x, y) meets the plane z = `planeZ`, or nullopt when the hit lies within `margin` of the
		// edge of the square |x|, |y| <= `halfSize` (or outside it).
		std::optional<glm::dvec3> HitSquare(const CameraData& camera, uint32_t x, uint32_t y, double planeZ, double halfSize, double margin)
		{
			const Result<RenderRay> ray = ComputeViewPixelRay(camera, x, y);
			REQUIRE_MESSAGE(ray.has_value(), ray.error().ToString());
			const glm::dvec3 origin(ray->Origin);
			const glm::dvec3 direction(ray->Direction);
			const glm::dvec3 hit = origin + direction * ((planeZ - origin.z) / direction.z);
			if (std::max(std::abs(hit.x), std::abs(hit.y)) > halfSize - margin)
				return std::nullopt;
			return hit;
		}

		// Whether `actual` is within `tolerance` of `expected` relative to its size (at least 1e-3 absolute), channel by channel.
		bool IsNear(const glm::dvec3& actual, const glm::dvec3& expected, double tolerance)
		{
			for (glm::length_t channel = 0; channel < 3; ++channel)
			{
				if (std::abs(actual[channel] - expected[channel]) > std::max(std::abs(expected[channel]) * tolerance, 1e-3))
					return false;
			}
			return true;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Fog: MakeFogConstants enables the fog only in a fogged view, and folds the camera, the sun and the ambient in")
		{
			CameraData camera;
			camera.Position = glm::vec3(1.0f, 2.0f, 3.0f);
			camera.View = glm::lookAt(camera.Position, glm::vec3(1.0f, 2.0f, -7.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const RenderFog fog = MakeTestFog();
			EnvironmentConstants environment;
			environment.AmbientColor = glm::vec3(0.5f, 0.25f, 1.0f);
			const std::array<LightData, 3> lights = {
				LightData{ .Type = RenderLightType::Point, .Color = glm::vec3(1.0f, 0.0f, 0.0f), .Intensity = 9.0f },
				LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(0.5f, 1.0f, 1.0f), .Intensity = 4.0f, .Direction = glm::vec3(0.0f, -2.0f, 0.0f) },
				LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(1.0f), .Intensity = 1.0f, .Direction = glm::vec3(1.0f, 0.0f, 0.0f) },
			};

			FogConstants constants = MakeFogConstants(fog, camera, lights, 1, environment, true);
			CHECK(constants.Enabled == 1U);
			CHECK(constants.FogSkybox == 1U);
			CHECK(constants.Orthographic == 0U);
			CHECK(constants.Color == fog.Color);
			CHECK(constants.Density == fog.Density);
			CHECK(constants.StartDistance == fog.StartDistance);
			CHECK(constants.MaxOpacity == fog.MaxOpacity);
			CHECK(constants.HeightDensity == fog.HeightDensity);
			CHECK(constants.HeightFalloff == fog.HeightFalloff);
			CHECK(constants.BaseHeight == fog.BaseHeight);
			CHECK(constants.SunGlowExponent == fog.SunGlowExponent);
			CHECK(constants.CameraPosition == camera.Position);
			CHECK(glm::length(constants.ViewForward - glm::vec3(0.0f, 0.0f, -1.0f)) < 1e-6f);
			// The first directional light, normalized and reversed, times the glow.
			CHECK(glm::length(constants.SunDirection - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-6f);
			CHECK(glm::length(constants.SunColor - glm::vec3(0.5f, 1.0f, 1.0f) * 4.0f * fog.SunGlow) < 1e-6f);

			// Not a fogged view, or no fog: disabled, every other member filled.
			CHECK(MakeFogConstants(fog, camera, lights, 1, environment, false).Enabled == 0U);
			RenderFog none = fog;
			none.Enabled = false;
			constants = MakeFogConstants(none, camera, lights, 1, environment, true);
			CHECK(constants.Enabled == 0U);
			CHECK(constants.Density == fog.Density);

			// The ambient tint: the constant ambient without a map, the SH9's mean with one.
			RenderFog tinted = fog;
			tinted.AmbientTint = 0.5f;
			constants = MakeFogConstants(tinted, camera, lights, 1, environment, true);
			CHECK(glm::length(constants.Color - fog.Color * glm::mix(glm::vec3(1.0f), environment.AmbientColor, 0.5f)) < 1e-6f);
			environment.HasEnvironment = 1;
			environment.IrradianceSH9[0] = glm::vec4(2.0f, 1.0f, 0.5f, 0.0f);
			constants = MakeFogConstants(tinted, camera, lights, 1, environment, true);
			const glm::vec3 shMean = glm::vec3(2.0f, 1.0f, 0.5f) * 0.282094791773878f;
			CHECK(glm::length(constants.Color - fog.Color * glm::mix(glm::vec3(1.0f), shMean, 0.5f)) < 1e-6f);

			// M45: the glow follows the light it is given, here the second directional light.
			constants = MakeFogConstants(fog, camera, lights, 2, environment, true);
			CHECK(glm::length(constants.SunDirection - glm::vec3(-1.0f, 0.0f, 0.0f)) < 1e-6f);
			CHECK(glm::length(constants.SunColor - glm::vec3(1.0f) * fog.SunGlow) < 1e-6f);

			// No glow light, one that is not directional, or an index outside the list: no glow. An orthographic camera is flagged.
			camera.ProjectionKind = RenderProjection::Orthographic;
			for (const std::optional<size_t> glow : { std::optional<size_t>(), std::optional<size_t>(0), std::optional<size_t>(3) })
			{
				constants = MakeFogConstants(fog, camera, lights, glow, environment, true);
				CHECK(constants.SunColor == glm::vec3(0.0f));
				CHECK(constants.SunDirection == glm::vec3(0.0f, 1.0f, 0.0f));
				CHECK(constants.Orthographic == 1U);
			}
		}

		TEST_CASE("Fog: with a Sky, the glow follows its Sun")
		{
			// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3): a moon placed above the sun in the hierarchy took the
			// glow before; with a Sky, its Sun owns it (GetSkyGlowLight), and without one the first directional light still does.
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.View = glm::lookAt(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			snapshot.Fog = MakeTestFog();
			const UUID moon(11);
			const UUID sun(22);
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(0.2f, 0.2f, 0.3f), .Intensity = 0.1f, .Direction = glm::vec3(0.0f, -1.0f, 0.0f), .Entity = moon });
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional, .Color = glm::vec3(1.0f, 0.9f, 0.8f), .Intensity = 5.0f, .Direction = glm::normalize(glm::vec3(1.0f, -1.0f, 0.0f)), .Entity = sun });

			REQUIRE(GetSkyGlowLight(snapshot) == std::optional<size_t>(0));
			FogConstants constants = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			CHECK(glm::length(constants.SunDirection - glm::vec3(0.0f, 1.0f, 0.0f)) < 1e-6f);

			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = sun;
			snapshot.Sky.Moon = moon;
			snapshot.Sky.DriveLights = false;
			ResolveSkyLights(snapshot);
			REQUIRE(GetSkyGlowLight(snapshot) == std::optional<size_t>(1));
			constants = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			CHECK(glm::length(constants.SunDirection - glm::normalize(glm::vec3(-1.0f, 1.0f, 0.0f))) < 1e-6f);
			CHECK(glm::length(constants.SunColor - glm::vec3(1.0f, 0.9f, 0.8f) * 5.0f * snapshot.Fog.SunGlow) < 1e-5f);

			// With DriveLights, the glow takes the Sun's driven colour: dimmer than authored, and redder (the transmittance).
			snapshot.Sky.DriveLights = true;
			ApplySkyLightDriving(snapshot);
			constants = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			const glm::vec3 authored = glm::vec3(1.0f, 0.9f, 0.8f) * 5.0f * snapshot.Fog.SunGlow;
			CHECK(constants.SunColor.r < authored.r);
			CHECK(constants.SunColor.b / authored.b < constants.SunColor.r / authored.r);
			CHECK(constants.SunColor.b > 0.0f);

			// A Sky whose Sun names no light leaves the glow to the first directional light.
			snapshot.Sky.Sun = UUID(33);
			ResolveSkyLights(snapshot);
			CHECK(GetSkyGlowLight(snapshot) == std::optional<size_t>(0));
		}

		TEST_CASE("Fog: MakeFogConstants makes non-finite and out-of-range values usable")
		{
			const float nan = std::numeric_limits<float>::quiet_NaN();
			const float infinity = std::numeric_limits<float>::infinity();
			CameraData camera;
			camera.Position = glm::vec3(nan, 0.0f, 0.0f);
			camera.View = glm::mat4(nan);
			const RenderFog defaults;
			const RenderFog broken{ .Enabled = true,
				.Color = glm::vec3(-1.0f, infinity, 0.5f),
				.AmbientTint = nan,
				.Density = 50.0f,
				.StartDistance = -3.0f,
				.MaxOpacity = 2.0f,
				.HeightDensity = nan,
				.HeightFalloff = 0.0f,
				.BaseHeight = infinity,
				.SunGlow = -1.0f,
				.SunGlowExponent = 1e9f };
			const std::array<LightData, 1> lights = { LightData{ .Type = RenderLightType::Directional, .Intensity = nan, .Direction = glm::vec3(0.0f) } };
			const FogConstants constants = MakeFogConstants(broken, camera, lights, 0, EnvironmentConstants{}, true);
			CHECK(constants.Color == glm::vec3(0.0f, 0.0f, 0.5f));
			CHECK(constants.Density == 10.0f);
			CHECK(constants.StartDistance == 0.0f);
			CHECK(constants.MaxOpacity == 1.0f);
			CHECK(constants.HeightDensity == defaults.HeightDensity);
			CHECK(constants.HeightFalloff == 0.001f);
			CHECK(constants.BaseHeight == defaults.BaseHeight);
			CHECK(constants.SunGlowExponent == 1000.0f);
			CHECK(constants.CameraPosition == glm::vec3(0.0f));
			CHECK(constants.ViewForward == glm::vec3(0.0f, 0.0f, -1.0f));
			// A light without a direction gives no glow.
			CHECK(constants.SunColor == glm::vec3(0.0f));
		}

		TEST_CASE("Fog: a fogged mesh shows the reference fog at its view depth, in both projections" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			for (const RenderProjection projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				CAPTURE(static_cast<int>(projection));
				const RenderSnapshot snapshot = MakeFogSnapshot(fixture, projection, MakeTestFog());
				fixture.Submit(snapshot);
				const std::vector<glm::dvec4> texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
				const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
				REQUIRE(fog.Enabled == 1U);
				size_t compared = 0;
				size_t mismatched = 0;
				for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
				{
					for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
					{
						const std::optional<glm::dvec3> hit = HitSquare(snapshot.Camera, x, y, -3.0, 1.0, 0.05);
						if (!hit)
							continue;
						const glm::dvec4 reference = Test::EvaluateFog(fog, *hit);
						const glm::dvec3 expected = glm::dvec3(Emission) * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
						++compared;
						mismatched += IsNear(glm::dvec3(texels[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]), expected, 4e-3) ? 0U : 1U;
					}
				}
				CAPTURE(compared);
				CHECK(compared > 400);
				CHECK(mismatched == 0);
			}
		}

		TEST_CASE("Fog: a camera high above a thin layer of height fog sees it below, in both projections" * doctest::test_suite(Test::GpuSuite))
		{
			// The camera looks down at the quad from 4 m up, onto a layer whose density falls by e every 3.3 cm above
			// BaseHeight: the density where each path begins, high above the layer, is below binary32's range, so the shaders
			// integrate from the path's lower end (Common/Fog.slang), and the quad's lower part is all but hidden.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			for (const RenderProjection projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				CAPTURE(static_cast<int>(projection));
				RenderFog layer = MakeTestFog();
				layer.Density = 0.0f;
				layer.StartDistance = 0.0f;
				layer.MaxOpacity = 0.95f;
				layer.HeightDensity = 10.0f;
				layer.HeightFalloff = 30.0f;
				layer.BaseHeight = -0.5f;
				layer.SunGlow = 0.0f;
				RenderSnapshot snapshot = MakeFogSnapshot(fixture, projection, layer);
				snapshot.Camera.Position = glm::vec3(0.0f, 4.0f, 0.0f);
				snapshot.Camera.View = glm::lookAt(snapshot.Camera.Position, glm::vec3(0.0f, 0.0f, -3.0f), glm::vec3(0.0f, 1.0f, 0.0f));
				fixture.Submit(snapshot);
				const std::vector<glm::dvec4> texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
				const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
				size_t compared = 0;
				size_t mismatched = 0;
				double mostFog = 0.0;
				for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
				{
					for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
					{
						const std::optional<glm::dvec3> hit = HitSquare(snapshot.Camera, x, y, -3.0, 1.0, 0.05);
						if (!hit)
							continue;
						const glm::dvec4 reference = Test::EvaluateFog(fog, *hit);
						const glm::dvec3 expected = glm::dvec3(Emission) * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
						mostFog = std::max(mostFog, reference.a);
						++compared;
						mismatched += IsNear(glm::dvec3(texels[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]), expected, 4e-3) ? 0U : 1U;
					}
				}
				CAPTURE(compared);
				CHECK(compared > 100);
				CHECK(mostFog > 0.9);
				CHECK(mismatched == 0);
			}
		}

		TEST_CASE("Fog: a transparent surface is fogged before it blends over the fogged surface behind it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
			MeshDrawItem pane = snapshot.Meshes.front();
			pane.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -2.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.4f));
			pane.Materials = { TransparentMaterialHandle };
			pane.Entity = UUID(901);
			pane.PickId = 0;
			snapshot.Meshes.push_back(pane);
			fixture.Submit(snapshot);
			const std::vector<glm::dvec4> texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			size_t compared = 0;
			size_t mismatched = 0;
			for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
			{
				for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
				{
					const std::optional<glm::dvec3> front = HitSquare(snapshot.Camera, x, y, -2.0, 0.4, 0.03);
					const std::optional<glm::dvec3> back = HitSquare(snapshot.Camera, x, y, -3.0, 1.0, 0.05);
					if (!front || !back)
						continue;
					const glm::dvec4 frontFog = Test::EvaluateFog(fog, *front);
					const glm::dvec4 backFog = Test::EvaluateFog(fog, *back);
					const glm::dvec3 behind = glm::dvec3(Emission) * (1.0 - backFog.a) + glm::dvec3(backFog) * backFog.a;
					const glm::dvec3 fogged = glm::dvec3(0.1, 0.6, 0.2) * (1.0 - frontFog.a) + glm::dvec3(frontFog) * frontFog.a;
					++compared;
					mismatched += IsNear(glm::dvec3(texels[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]), fogged * 0.5 + behind * 0.5, 5e-3) ? 0U : 1U;
				}
			}
			CAPTURE(compared);
			CHECK(compared > 100);
			CHECK(mismatched == 0);
		}

		TEST_CASE("Fog: a fog of zero density changes no pixel" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, std::nullopt);
			snapshot.Environment = RenderEnvironment{ .Environment = SkyEnvironmentHandle, .Intensity = 1.0f, .ShowSkybox = true };
			snapshot.Camera.ClearToSkybox = true;
			const Image unfogged = fixture.Render(snapshot);
			const std::vector<glm::dvec4> unfoggedColor = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			RenderFog clear = MakeTestFog();
			clear.Density = 0.0f;
			clear.HeightDensity = 0.0f;
			clear.SunGlow = 4.0f;
			snapshot.Fog = clear;
			const Image fogged = fixture.Render(snapshot);
			CHECK(fogged.Pixels == unfogged.Pixels);
			CHECK(ReadSceneColor(gpu.GetDevice(), fixture.Renderer()) == unfoggedColor);
		}

		TEST_CASE("Fog: the skybox is fogged as the sky at infinite depth only with FogSkybox" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
			snapshot.Meshes.clear();
			snapshot.Environment = RenderEnvironment{ .Environment = SkyEnvironmentHandle, .Intensity = 1.0f, .ShowSkybox = true };
			snapshot.Camera.ClearToSkybox = true;
			// Height fog alone, with the camera looking level, so the upper sky climbs out of it and the lower sky is fully fogged.
			snapshot.Fog.Density = 0.0f;
			snapshot.Fog.BaseHeight = 0.0f;
			snapshot.Fog.HeightFalloff = 0.8f;
			snapshot.Fog.HeightDensity = 0.3f;

			fixture.Submit(snapshot);
			std::vector<glm::dvec4> texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			size_t mismatched = 0;
			for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
			{
				for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
				{
					const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
					REQUIRE(ray.has_value());
					const glm::dvec4 reference = Test::EvaluateSkyFog(fog, glm::dvec3(ray->Direction), glm::dvec3(snapshot.Camera.Position));
					const glm::dvec3 expected = SkyRadiance * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
					// Rows next to the horizon change fastest; the sky's fog there is compared within a wider tolerance.
					const double tolerance = std::abs(static_cast<double>(ray->Direction.y)) < 0.05 ? 3e-2 : 5e-3;
					mismatched += IsNear(glm::dvec3(texels[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]), expected, tolerance) ? 0U : 1U;
				}
			}
			CHECK(mismatched == 0);

			// Distance fog makes the whole sky MaxOpacity fog, and without FogSkybox the sky is left alone.
			snapshot.Fog.Density = 0.05f;
			fixture.Submit(snapshot);
			texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			const glm::dvec4 dense = Test::EvaluateSkyFog(MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true),
				glm::dvec3(0.0, 0.0, -1.0), glm::dvec3(0.0));
			CHECK(dense.a == doctest::Approx(0.9));
			CHECK(IsNear(glm::dvec3(texels[32 * 64 + 32]), SkyRadiance * 0.1 + glm::dvec3(dense) * 0.9, 5e-3));
			snapshot.Fog.FogSkybox = false;
			fixture.Submit(snapshot);
			texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			CHECK(IsNear(glm::dvec3(texels[32 * 64 + 32]), SkyRadiance, 2e-3));
			CHECK(IsNear(glm::dvec3(texels[5 * 64 + 7]), SkyRadiance, 2e-3));
		}

		// M32's milestone review: a view that clears to a colour (or to a skybox it has none of) has that colour for its sky,
		// and with FogSkybox its cleared pixels are fogged as the sky, so a void below a bridge or the world's edge fades into
		// the fog; without FogSkybox, or without fog, they keep the clear colour exactly. The mesh is untouched by the mode.
		TEST_CASE("Fog: a view clearing to a colour fogs its cleared pixels as the sky only with FogSkybox" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			const glm::vec3 clearColor(0.2f, 0.05f, 0.4f);
			for (const bool clearToSkybox : { false, true })
			{
				CAPTURE(clearToSkybox);
				RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
				snapshot.Camera.ClearColor = clearColor;
				// A skybox clear without an environment map clears to the colour too.
				snapshot.Camera.ClearToSkybox = clearToSkybox;
				snapshot.Fog.Density = 0.0f;
				snapshot.Fog.BaseHeight = 0.0f;
				snapshot.Fog.HeightFalloff = 0.8f;
				snapshot.Fog.HeightDensity = 0.3f;
				fixture.Submit(snapshot);
				const std::vector<glm::dvec4> fogged = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
				const FogConstants fog = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
				size_t compared = 0;
				size_t mismatched = 0;
				for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
				{
					for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
					{
						// The cleared pixels: those whose ray misses the quad (with a margin at its edge).
						if (HitSquare(snapshot.Camera, x, y, -3.0, 1.1, 0.0))
							continue;
						const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
						REQUIRE(ray.has_value());
						const glm::dvec4 reference = Test::EvaluateSkyFog(fog, glm::dvec3(ray->Direction), glm::dvec3(snapshot.Camera.Position));
						const glm::dvec3 expected = glm::dvec3(clearColor) * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
						const double tolerance = std::abs(static_cast<double>(ray->Direction.y)) < 0.05 ? 3e-2 : 5e-3;
						++compared;
						mismatched += IsNear(glm::dvec3(fogged[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]), expected, tolerance) ? 0U : 1U;
					}
				}
				CHECK(compared > 1000);
				CHECK(mismatched == 0);
				// The lower sky is deep in the height fog: there the cleared pixel is all but the fog's colour.
				const glm::dvec3 lowest = fogged[static_cast<size_t>(snapshot.Camera.ViewportHeight - 1) * snapshot.Camera.ViewportWidth];
				CHECK_FALSE(IsNear(lowest, glm::dvec3(clearColor), 5e-2));

				// Without FogSkybox, and without fog, the cleared pixels keep the clear colour.
				for (const bool withFog : { true, false })
				{
					CAPTURE(withFog);
					RenderSnapshot plain = snapshot;
					plain.Fog.FogSkybox = false;
					plain.Fog.Enabled = withFog;
					fixture.Submit(plain);
					const std::vector<glm::dvec4> texels = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
					CHECK(IsNear(glm::dvec3(texels.front()), glm::dvec3(clearColor), 1e-3));
					CHECK(texels.back() == texels.front());
				}
			}
		}

		// The review of W45.1 (finding 2): with a Sky, the view's fog glows toward its Sun (GetSkyGlowLight in Prepare), not
		// toward the light list's first directional light. A Moon light first and the Sun second, toward different sides of the
		// view: the cleared pixels, fogged as the sky with FogSkybox, match the reference fog glowing toward the Sun, and many
		// differ from the one glowing toward the Moon.
		TEST_CASE("Fog: with a Sky, the view's sky glows toward its Sun" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			const glm::vec3 clearColor(0.2f, 0.05f, 0.4f);
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
			snapshot.Camera.ClearColor = clearColor;
			snapshot.Camera.ClearToSkybox = false;
			snapshot.Fog.Density = 0.0f;
			snapshot.Fog.BaseHeight = 0.0f;
			snapshot.Fog.HeightFalloff = 0.8f;
			snapshot.Fog.HeightDensity = 0.3f;
			snapshot.Fog.SunGlow = 2.0f;
			// MakeFogSnapshot's light (behind the quad, low on the left) is the Moon light; the Sun, up on the right, comes second.
			snapshot.Lights[0].Entity = UUID(81);
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
				.Color = glm::vec3(1.0f, 0.6f, 0.3f),
				.Intensity = 3.0f,
				.Direction = -glm::normalize(glm::vec3(0.45f, 0.35f, -1.0f)),
				.Entity = UUID(82) });
			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = UUID(82);
			snapshot.Sky.Moon = UUID(81);
			snapshot.Sky.DriveLights = false;
			ResolveSkyLights(snapshot);
			REQUIRE(GetSkyGlowLight(snapshot) == std::optional<size_t>(1));
			fixture.Submit(snapshot);
			const std::vector<glm::dvec4> fogged = ReadSceneColor(gpu.GetDevice(), fixture.Renderer());
			const FogConstants towardSun = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, GetSkyGlowLight(snapshot), EnvironmentConstants{}, true);
			const FogConstants towardMoon = MakeFogConstants(snapshot.Fog, snapshot.Camera, snapshot.Lights, std::optional<size_t>(0), EnvironmentConstants{}, true);
			size_t compared = 0;
			size_t mismatched = 0;
			size_t notTowardMoon = 0;
			for (uint32_t y = 0; y < snapshot.Camera.ViewportHeight; ++y)
			{
				for (uint32_t x = 0; x < snapshot.Camera.ViewportWidth; ++x)
				{
					if (HitSquare(snapshot.Camera, x, y, -3.0, 1.1, 0.0))
						continue;
					const Result<RenderRay> ray = ComputeViewPixelRay(snapshot.Camera, x, y);
					REQUIRE(ray.has_value());
					const auto expected = [&clearColor, &ray, &snapshot](const FogConstants& fog)
					{
						const glm::dvec4 reference = Test::EvaluateSkyFog(fog, glm::dvec3(ray->Direction), glm::dvec3(snapshot.Camera.Position));
						return glm::dvec3(clearColor) * (1.0 - reference.a) + glm::dvec3(reference) * reference.a;
					};
					const double tolerance = std::abs(static_cast<double>(ray->Direction.y)) < 0.05 ? 3e-2 : 5e-3;
					const glm::dvec3 texel(fogged[static_cast<size_t>(y) * snapshot.Camera.ViewportWidth + x]);
					++compared;
					mismatched += IsNear(texel, expected(towardSun), tolerance) ? 0U : 1U;
					notTowardMoon += IsNear(texel, expected(towardMoon), 5e-2) ? 0U : 1U;
				}
			}
			CHECK(compared > 1000);
			CHECK(mismatched == 0);
			CHECK(notTowardMoon > 200);
		}

		TEST_CASE("Fog: the debug views and the wireframe are never fogged" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			for (const RenderDebugView view : { RenderDebugView::Albedo, RenderDebugView::Emissive, RenderDebugView::Normals })
			{
				CAPTURE(RenderDebugViewToString(view));
				RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
				snapshot.DebugView = view;
				const Image fogged = fixture.Render(snapshot);
				snapshot.Fog = {};
				CHECK(fixture.Render(snapshot).Pixels == fogged.Pixels);
			}
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
			snapshot.Flags = RenderViewFlags::Wireframe;
			const Image fogged = fixture.Render(snapshot);
			snapshot.Fog = {};
			CHECK(fixture.Render(snapshot).Pixels == fogged.Pixels);
		}

		TEST_CASE("Fog: a fogged renderer recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			// The fog's constants are a per-view buffer of the renderer (place 2), created again with the others by the recovery.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			PublishFogAssets(fixture);
			RenderSnapshot snapshot = MakeFogSnapshot(fixture, RenderProjection::Perspective, MakeTestFog());
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
		}
	}

}
