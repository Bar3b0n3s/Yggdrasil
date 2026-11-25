#include "TestsPCH.h"

#include "Engine/Renderer/ProbeRelight.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IrradianceVolumeData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SkyLighting.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"
#include "Support/BufferReadback.h"
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
#include <functional>
#include <numbers>
#include <span>
#include <utility>
#include <vector>

// Relit probes (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"; Renderer/ProbeRelight.h): the
// CPU relight of a probe, the GPU's (Passes/Sky.slang's CSProbeRelight) against it, and a view whose relightable volume and
// reflection probe follow the sun with no bake between.

namespace Engine {

	namespace {

		using ProbeSH9 = std::array<glm::vec3, 9>;

		constexpr uint32_t CaptureSize = 16;
		constexpr AssetHandle RelitVolumeHandle{ 0x9b01 };
		constexpr AssetHandle BakedVolumeHandle{ 0x9b02 };
		constexpr AssetHandle ChromeHandle{ 0x9b03 };
		constexpr AssetHandle ReflectionHandle{ 0x9b04 };
		constexpr AssetHandle OuterVolumeHandle{ 0x9b05 };
		// Y00, the constant SH basis function.
		constexpr double ShConstant = 0.28209479177387814;

		// A capture of `size` texels a face whose texel in direction d has the radiance `radiance(d)` and the visibility
		// `visible(d)` (ProbeCubeCapture's faces and texel order, Test::CubeTexelDirection's directions).
		ProbeCubeCapture MakeCapture(const std::function<glm::dvec3(const glm::dvec3&)>& radiance, const std::function<bool(const glm::dvec3&)>& visible)
		{
			ProbeCubeCapture capture;
			capture.FaceSize = CaptureSize;
			for (uint32_t face = 0; face < 6; ++face)
			{
				for (uint32_t row = 0; row < CaptureSize; ++row)
				{
					for (uint32_t column = 0; column < CaptureSize; ++column)
					{
						const glm::dvec3 direction = Test::CubeTexelDirection(face, column, row, CaptureSize);
						capture.Faces[face].emplace_back(glm::vec3(radiance(direction)), 1.0f);
						capture.Visibility[face].push_back(visible(direction) ? 1.0f : 0.0f);
					}
				}
			}
			return capture;
		}

		// A sky brighter and bluer above than at the horizon, with a dim ground: a radiance with every band of SH9.
		glm::dvec3 SkyRadiance(const glm::dvec3& direction)
		{
			if (direction.y < 0.0)
				return glm::dvec3(0.05, 0.04, 0.03);
			return glm::dvec3(0.4, 0.6, 1.0) * (0.5 + direction.y) + glm::dvec3(0.3, 0.1, 0.0) * std::max(direction.x, 0.0);
		}

		// The sky's radiance SH9 of a capture of it: its irradiance projection divided by each band's factor.
		ProbeSH9 ProjectRadiance(const ProbeCubeCapture& capture)
		{
			const ProbeSH9 irradiance = ProjectCubeToIrradianceSH9(capture);
			const std::array<double, 3> factors = GetIrradianceBandFactors();
			ProbeSH9 radiance{};
			for (size_t index = 0; index < radiance.size(); ++index)
				radiance[index] = glm::vec3(glm::dvec3(irradiance[index]) / factors[GetShBand(index)]);
			return radiance;
		}

		// The cave's transfer: its probe sees the sky only through a mouth toward +Z and upward (z > 0.5, y > 0).
		std::array<float, IrradianceTransferCount> MakeCaveTransfer()
		{
			return ProjectCubeToSkyTransfer(MakeCapture([](const glm::dvec3&)
			{
				return glm::dvec3(0.0);
			}, [](const glm::dvec3& direction)
			{
				return direction.z > 0.5 && direction.y > 0.0;
			}));
		}

		ProbeSH9 MakeLamp(const glm::vec3& radiance)
		{
			ProbeSH9 coefficients{};
			coefficients[0] = radiance / static_cast<float>(ShConstant);
			return coefficients;
		}

		// A 2 x 2 x 2 volume of 4 m whose probes are `probe` and, when `relight`, the relight block `block`.
		AssetRef<Asset> MakeVolume(const ProbeSH9& probe, const IrradianceProbeRelight& block, bool relight)
		{
			IrradianceVolumeData volume;
			volume.Size = glm::vec3(4.0f);
			volume.Probes.assign(8, IrradianceProbe{ .SH9 = probe });
			if (relight)
				volume.Relight.assign(8, block);
			return CreateRef<IrradianceVolumeData>(std::move(volume));
		}

		// A 2 x 2 x 2 volume of `size` metres whose probe i has the relight block `blocks[i]` and, as its bake, that block relit
		// at `sky` (RelightIrradianceSH9, what lighting.bake writes).
		AssetRef<Asset> MakeBakedVolume(float size, const std::array<IrradianceProbeRelight, 8>& blocks, const ProbeSH9& sky)
		{
			IrradianceVolumeData volume;
			volume.Size = glm::vec3(size);
			for (const IrradianceProbeRelight& block : blocks)
			{
				volume.Probes.push_back(IrradianceProbe{ .SH9 = RelightIrradianceSH9(block.Local, block.Transfer, sky) });
				volume.Relight.push_back(block);
			}
			return CreateRef<IrradianceVolumeData>(std::move(volume));
		}

		// Probe 3, at (1, 1, 0) of a 2 x 2 x 2 grid, sees the sky through the cave's mouth; the other seven see none of it (`open`
		// false: none of the eight). Each holds the lamp `lamp`.
		std::array<IrradianceProbeRelight, 8> MakeCornerBlocks(const ProbeSH9& lamp, bool open)
		{
			std::array<IrradianceProbeRelight, 8> blocks{};
			for (size_t probe = 0; probe < blocks.size(); ++probe)
			{
				blocks[probe].Local = lamp;
				if (open && probe == 3)
					blocks[probe].Transfer = MakeCaveTransfer();
			}
			return blocks;
		}

		// An environment whose specular cube is a uniform `radiance` (8², 4 levels), with a black skybox and no irradiance.
		AssetRef<Asset> MakeUniformEnvironment(double radiance)
		{
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = 1, .MipCount = 1, .Texels = Buffer(ComputeCubeMapByteSize(1, 1), std::byte{ 0 }) };
			CubeMapData specular{ .FaceSize = 8, .MipCount = 4, .Texels = Buffer(ComputeCubeMapByteSize(8, 4)) };
			const uint16_t value = Test::DoubleToHalf(radiance);
			const std::array<uint16_t, 4> texel = { value, value, value, Test::DoubleToHalf(1.0) };
			for (size_t offset = 0; offset + sizeof(texel) <= specular.Texels.size(); offset += sizeof(texel))
				std::memcpy(specular.Texels.data() + offset, texel.data(), sizeof(texel));
			environment.Specular = std::move(specular);
			return CreateRef<EnvironmentData>(std::move(environment));
		}

		// The fixture's quad 3 m ahead (facing +Z, toward the camera) under a Sky whose sun stands `elevation` degrees up toward
		// -Z, behind the quad, so the quad is lit by its probes and the sky's image-based light alone.
		RenderSnapshot MakeRelitSnapshot(Test::SceneRendererIntegrationFixture& fixture, float elevation)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Camera.ClearToSkybox = true;
			snapshot.Camera.FarClip = 1000.0f;
			snapshot.Post.SsrEnabled = false;
			const float radians = glm::radians(elevation);
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
				.Color = glm::vec3(1.0f, 0.95f, 0.9f),
				.Intensity = 6.0f,
				.Direction = -glm::vec3(0.0f, std::sin(radians), -std::cos(radians)),
				.LightAngle = 0.53f,
				.Entity = UUID(77) });
			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = UUID(77);
			snapshot.Sky.DriveLights = false;
			ResolveSkyLights(snapshot);
			return snapshot;
		}

		// The volume `data` of `size` metres about `centre` (by default 4 m about the quad).
		IrradianceVolumeItem MakeVolumeItem(AssetHandle data, const glm::vec3& centre = glm::vec3(0.0f, 0.0f, -3.0f), float size = 4.0f, UUID entity = UUID(31))
		{
			return IrradianceVolumeItem{ .World = glm::translate(glm::mat4(1.0f), centre), .Size = glm::vec3(size), .Data = data, .BlendDistance = 0.0f, .Entity = entity };
		}

		// A reflection probe of the uniform environment `data`, its 30 m box about `centre` (around the quad), not box-projected.
		ReflectionProbeItem MakeReflectionItem(AssetHandle data, const glm::vec3& centre)
		{
			return ReflectionProbeItem{ .World = glm::translate(glm::mat4(1.0f), centre),
				.Size = glm::vec3(30.0f),
				.Data = data,
				.BoxProjection = false,
				.BlendDistance = 0.0f,
				.Entity = UUID(32) };
		}

		// The chrome quad's scene (ChromeHandle published): MakeRelitSnapshot's sky with the quad of the chrome material, casting
		// no shadow, so it shows its reflection alone (a metal has no diffuse term).
		RenderSnapshot MakeChromeSnapshot(Test::SceneRendererIntegrationFixture& fixture, float elevation, uint64_t frame)
		{
			RenderSnapshot snapshot = MakeRelitSnapshot(fixture, elevation);
			snapshot.Meshes[0].Materials = { ChromeHandle };
			snapshot.Meshes[0].CastShadows = false;
			snapshot.FrameIndex = frame;
			return snapshot;
		}

		// Publishes the chrome material and, as ReflectionHandle, a uniform environment of radiance 0.5.
		void PublishChrome(Test::SceneRendererIntegrationFixture& fixture)
		{
			auto chrome = CreateRef<MaterialData>();
			chrome->BaseColor = glm::vec4(1.0f);
			chrome->Metallic = 1.0f;
			chrome->Roughness = 0.2f;
			fixture.Assets().Publish(ChromeHandle, chrome);
			fixture.Assets().Publish(ReflectionHandle, MakeUniformEnvironment(0.5));
		}

		// The mean linear green of SceneColor's centre square of half the view's size (the quad).
		double MeanCentreGreen(GraphicsDevice& device, SceneRenderer& renderer)
		{
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*renderer.GetSceneColorTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			double sum = 0.0;
			uint32_t count = 0;
			for (uint32_t y = image->Height * 3 / 8; y < image->Height * 5 / 8; ++y)
			{
				const std::span<const std::byte> row = image->GetRow(y);
				for (uint32_t x = image->Width * 3 / 8; x < image->Width * 5 / 8; ++x)
				{
					uint16_t green = 0;
					std::memcpy(&green, row.data() + static_cast<size_t>(x) * 8 + 2, sizeof(green));
					sum += Test::HalfToDouble(green);
					++count;
				}
			}
			return sum / static_cast<double>(count);
		}

		// The view's sky radiance SH9 (before Intensity) as its last render made it, read back from its totals.
		ProbeSH9 ReadSkyRadiance(GraphicsDevice& device, SceneRenderer& renderer)
		{
			nvrhi::IBuffer* totals = renderer.GetSkyRadianceTotals();
			REQUIRE(totals != nullptr);
			const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(device, *totals, 10 * sizeof(glm::vec4));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			std::array<glm::vec4, 10> values{};
			std::memcpy(values.data(), bytes->data(), sizeof(values));
			return MakeSkyRadianceSH9(values, 1.0f);
		}

		double Luminance(const glm::vec3& value)
		{
			return 0.2126 * value.r + 0.7152 * value.g + 0.0722 * value.b;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The transfer's index, the sky's normalization and the reflection scale (ProbeRelight.h).
		TEST_CASE("ProbeRelight: the transfer's upper triangle, the sky's totals and the reflection scale follow their rules")
		{
			CHECK(GetSkyTransferIndex(0, 0) == 0);
			CHECK(GetSkyTransferIndex(0, 8) == 8);
			CHECK(GetSkyTransferIndex(1, 1) == 9);
			CHECK(GetSkyTransferIndex(2, 1) == 10);
			CHECK(GetSkyTransferIndex(1, 2) == 10);
			CHECK(GetSkyTransferIndex(8, 8) == IrradianceTransferCount - 1);
			CHECK(GetShBand(0) == 0);
			CHECK(GetShBand(3) == 1);
			CHECK(GetShBand(4) == 2);
			std::array<glm::vec4, 10> totals{};
			for (size_t index = 0; index < 9; ++index)
				totals[index] = glm::vec4(static_cast<float>(index), 1.0f, 2.0f, 0.0f);
			totals[9].x = static_cast<float>(2.0 * std::numbers::pi);
			const ProbeSH9 sky = MakeSkyRadianceSH9(totals, 1.5f);
			// 4 pi over a solid angle of 2 pi doubles the sums, and Intensity 1.5 scales them.
			CHECK(sky[3].x == doctest::Approx(3.0 * 2.0 * 1.5));
			CHECK(sky[0].z == doctest::Approx(2.0 * 2.0 * 1.5));
			totals[9].x = 0.0f;
			CHECK(MakeSkyRadianceSH9(totals, 1.5f)[3] == glm::vec3(0.0f));
			CHECK(ComputeReflectionRelightScale(glm::vec3(0.5f), glm::vec3(1.0f)) == doctest::Approx(0.5));
			CHECK(ComputeReflectionRelightScale(glm::vec3(10.0f), glm::vec3(1.0f)) == MaxReflectionRelightScale);
			CHECK(ComputeReflectionRelightScale(glm::vec3(1.0f), glm::vec3(0.0f)) == 1.0f);
		}

		// Decision 3's projection: E = Local + conv(T S). A probe that sees the whole sky (T the identity, ProjectCubeToSkyTransfer
		// of a capture visible everywhere) relights to the sky's own irradiance, what ProjectCubeToIrradianceSH9 makes of a capture
		// of that sky; one that sees none of it (T zero) to its local light alone, exactly.
		TEST_CASE("ProbeRelight: an open probe relights to the sky's irradiance and a closed one to its local light")
		{
			const ProbeCubeCapture sky = MakeCapture(SkyRadiance, [](const glm::dvec3&)
			{
				return true;
			});
			const ProbeSH9 irradiance = ProjectCubeToIrradianceSH9(sky);
			const ProbeSH9 radiance = ProjectRadiance(sky);
			const std::array<float, IrradianceTransferCount> open = ProjectCubeToSkyTransfer(sky);
			const ProbeSH9 lamp = MakeLamp(glm::vec3(0.02f, 0.015f, 0.01f));

			const ProbeSH9 relit = RelightIrradianceSH9(ProbeSH9{}, open, radiance);
			for (size_t index = 0; index < 9; ++index)
			{
				CAPTURE(index);
				for (glm::length_t channel = 0; channel < 3; ++channel)
					CHECK(relit[index][channel] == doctest::Approx(irradiance[index][channel]).epsilon(0.01).scale(irradiance[0][channel]));
			}
			const ProbeSH9 openLit = RelightIrradianceSH9(lamp, open, radiance);
			CHECK(openLit[0].g == doctest::Approx(irradiance[0].g + lamp[0].g).epsilon(0.01));

			std::array<float, IrradianceTransferCount> closed{};
			CHECK(RelightIrradianceSH9(lamp, closed, radiance) == lamp);
			// The cave between: some of the sky, less than all of it.
			const ProbeSH9 cave = RelightIrradianceSH9(lamp, MakeCaveTransfer(), radiance);
			CHECK(cave[0].g > lamp[0].g);
			CHECK(cave[0].g < openLit[0].g);
		}

		// The GPU's relight (CSProbeRelight) of a cached relightable volume, its probes written from an offset into a buffer as
		// the view's IrradianceProbes copy, against RelightIrradianceSH9 of the sky's totals; then the reflection probes' scales:
		// one inside (the luminance ratio of the relit to the baked constant term, times its Intensity) and one outside (its
		// Intensity).
		TEST_CASE("ProbeRelight: the GPU relight matches its CPU reference" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture fixture(gpu, 16, 16);
			// The volume: each probe its own local light and a transfer from its own visibility.
			IrradianceVolumeData volume;
			volume.Size = glm::vec3(4.0f);
			volume.Probes.resize(8);
			volume.Relight.resize(8);
			for (size_t probe = 0; probe < 8; ++probe)
			{
				const double threshold = -0.8 + 0.2 * static_cast<double>(probe);
				volume.Relight[probe].Local = MakeLamp(glm::vec3(0.01f * static_cast<float>(probe + 1), 0.02f, 0.03f));
				volume.Relight[probe].Transfer = ProjectCubeToSkyTransfer(MakeCapture(SkyRadiance, [threshold](const glm::dvec3& direction)
				{
					return direction.y > threshold && direction.x < 0.7;
				}));
				volume.Probes[probe].SH9 = MakeLamp(glm::vec3(0.1f + 0.05f * static_cast<float>(probe)));
			}
			const IrradianceVolumeData expectedVolume = volume;
			fixture.Assets().Publish(RelitVolumeHandle, CreateRef<IrradianceVolumeData>(std::move(volume)));
			const GpuIrradianceVolume* mirror = fixture.Cache().GetIrradianceVolume(RelitVolumeHandle);
			REQUIRE(mirror != nullptr);
			REQUIRE(mirror->Relight != nullptr);
			REQUIRE(mirror->ConstantTerms.size() == 8);

			Result<Scope<SkyLightingPass>> pass = SkyLightingPass::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			Result<SkyViewResources> resources = CreateSkyViewResources(device);
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			PassBindingCache bindings;
			std::array<glm::vec4, 10> totals{};
			for (size_t index = 0; index < 9; ++index)
				totals[index] = glm::vec4(0.3f - 0.05f * static_cast<float>(index), 0.2f + 0.01f * static_cast<float>(index), 0.5f, 0.0f);
			totals[9].x = static_cast<float>(3.0 * std::numbers::pi);
			SkyConstants constants{};
			constants.Intensity = 1.5f;
			constexpr uint32_t First = 9;
			nvrhi::BufferDesc desc;
			desc.byteSize = (First + 8 * IrradianceProbeCoefficientCount) * sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.canHaveUAVs = true;
			desc.initialState = nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = "ProbeRelightTest.Coefficients";
			Result<nvrhi::BufferHandle> coefficients = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(coefficients.has_value(), coefficients.error().ToString());
			// Reflection probe 0 is near probe 5, its baked constant term's luminance; probe 1 is outside every relit volume.
			const glm::vec3 baked5 = mirror->ConstantTerms[5];
			const std::array<glm::vec4, 2> reflections = {
				glm::vec4(static_cast<float>(First + 5 * IrradianceProbeCoefficientCount), static_cast<float>(Luminance(baked5)), 2.0f, 0.0f),
				glm::vec4(-1.0f, 0.0f, 0.75f, 0.0f),
			};
			{
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
				nvrhi::ICommandList& commandList = **list;
				commandList.open();
				commandList.writeBuffer(resources->Constants, &constants, sizeof(constants));
				commandList.writeBuffer(resources->Totals, totals.data(), sizeof(totals));
				commandList.writeBuffer(resources->ReflectionRelights, reflections.data(), sizeof(reflections));
				const Status relit = (*pass)->RecordProbeRelight(commandList, bindings, *resources, *mirror->Relight, **coefficients, First, 8);
				REQUIRE_MESSAGE(relit.has_value(), relit.error().ToString());
				const Status scaled = (*pass)->RecordReflectionRelight(commandList, bindings, *resources, **coefficients, 2);
				REQUIRE_MESSAGE(scaled.has_value(), scaled.error().ToString());
				commandList.close();
				WaitForSubmission(device, device.ExecuteCommandList(commandList), "the relight test");
			}
			const Result<std::vector<std::byte>> read = Test::ReadBufferBytes(device, **coefficients, desc.byteSize);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			std::vector<glm::vec4> gpuProbes(desc.byteSize / sizeof(glm::vec4));
			std::memcpy(gpuProbes.data(), read->data(), desc.byteSize);
			const ProbeSH9 sky = MakeSkyRadianceSH9(totals, constants.Intensity);
			for (size_t probe = 0; probe < 8; ++probe)
			{
				CAPTURE(probe);
				const ProbeSH9 expected = RelightIrradianceSH9(expectedVolume.Relight[probe].Local, expectedVolume.Relight[probe].Transfer, sky);
				for (size_t index = 0; index < 9; ++index)
				{
					CAPTURE(index);
					const glm::vec4 actual = gpuProbes[First + probe * IrradianceProbeCoefficientCount + index];
					for (glm::length_t channel = 0; channel < 3; ++channel)
						CHECK(actual[channel] == doctest::Approx(expected[index][channel]).epsilon(1e-4).scale(1.0));
					CHECK(actual.w == 0.0f);
				}
			}
			const Result<std::vector<std::byte>> results = Test::ReadBufferBytes(device, *resources->Results, SkyResultsCount * sizeof(glm::vec4));
			REQUIRE_MESSAGE(results.has_value(), results.error().ToString());
			std::vector<glm::vec4> values(SkyResultsCount);
			std::memcpy(values.data(), results->data(), results->size());
			const ProbeSH9 relit5 = RelightIrradianceSH9(expectedVolume.Relight[5].Local, expectedVolume.Relight[5].Transfer, sky);
			CHECK(values[SkyResultsReflectionScales].x == doctest::Approx(2.0 * ComputeReflectionRelightScale(relit5[0], baked5)).epsilon(1e-4));
			CHECK(values[SkyResultsReflectionScales + 1].x == 0.75f);
			device.RunGarbageCollection();
		}

		// The acceptance's cave: a relightable volume whose probes see the sky through a mouth and hold a dim lamp of their own
		// lights the quad brightly at noon and dimly at night with no bake between, each render after the sky changed relighting
		// it (RenderStats::RelitIrradianceVolumes, the ProbeRelight row's relight dispatch) and a still sky not; the same probes
		// baked without a relight block keep their bake's light through the day. A black reflection probe around the quad takes
		// the sky's specular light away, so the quad shows its probes' diffuse light alone (its scale is the row's every frame).
		TEST_CASE("ProbeRelight: a cave darkens at night and lights at noon with no bake between" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			const ProbeSH9 lamp = MakeLamp(glm::vec3(0.005f));
			const IrradianceProbeRelight block{ .Local = lamp, .Transfer = MakeCaveTransfer() };
			fixture.Assets().Publish(RelitVolumeHandle, MakeVolume(lamp, block, true));
			fixture.Assets().Publish(BakedVolumeHandle, MakeVolume(lamp, block, false));
			fixture.Assets().Publish(ReflectionHandle, MakeUniformEnvironment(0.0));
			const auto render = [&fixture, &device](AssetHandle volume, float elevation, uint64_t frame)
			{
				RenderSnapshot snapshot = MakeRelitSnapshot(fixture, elevation);
				snapshot.IrradianceVolumes.push_back(MakeVolumeItem(volume));
				snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -10.0f)),
					.Size = glm::vec3(30.0f),
					.Data = ReflectionHandle,
					.BoxProjection = false,
					.BlendDistance = 0.0f,
					.Entity = UUID(32) });
				snapshot.FrameIndex = frame;
				fixture.Submit(snapshot);
				return MeanCentreGreen(device, fixture.Renderer());
			};
			const double noon = render(RelitVolumeHandle, 70.0f, 20);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.RelitIrradianceVolumes == 1U);
			// The row: the volume's relight and the reflection probe's scale.
			const RenderPassStats* relit = Test::SceneRendererIntegrationFixture::FindPass(stats, "ProbeRelight");
			REQUIRE(relit != nullptr);
			CHECK(relit->Dispatches == 2U);
			const double night = render(RelitVolumeHandle, -30.0f, 21);
			CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 1U);
			const double stillNight = render(RelitVolumeHandle, -30.0f, 22);
			stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.RelitIrradianceVolumes == 0U);
			// A still sky: the reflection probe's scale alone, every frame.
			const RenderPassStats* still = Test::SceneRendererIntegrationFixture::FindPass(stats, "ProbeRelight");
			REQUIRE(still != nullptr);
			CHECK(still->Dispatches == 1U);
			CHECK(stillNight == night);
			const double noonAgain = render(RelitVolumeHandle, 70.0f, 23);
			INFO("noon " << noon << ", night " << night << ", noon again " << noonAgain);
			CHECK(night > 0.0);
			CHECK(noon > 5.0 * night);
			CHECK(noonAgain == doctest::Approx(noon).epsilon(1e-6));
			// Baked without a relight block, the probes keep the lamp's light at noon as at night.
			const double bakedNoon = render(BakedVolumeHandle, 70.0f, 24);
			CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 0U);
			const double bakedNight = render(BakedVolumeHandle, -30.0f, 25);
			CHECK(bakedNoon == doctest::Approx(bakedNight).epsilon(1e-6));
			CHECK(bakedNight == doctest::Approx(night).epsilon(0.05).scale(0.0));
			device.RunGarbageCollection();
		}

		// Decision 3: a reflection probe whose centre lies in a relit volume takes its Intensity times the luminance ratio of the
		// volume's relit to its baked constant term at the probe nearest that centre. Baked at noon, a chrome quad reflecting
		// the probe's uniform cube keeps its noon reflection at noon and dims at night by that ratio. The volume's probes differ
		// (the W45.3 review's finding 2): only probe 3, at (1, 1, 0), sees the sky, so a centre nearest it dims while one
		// nearest a closed probe keeps its bake, and inside a larger relightable volume whose probes are all closed the smaller
		// one's probe still decides; a probe outside every relit volume keeps its bake's brightness at night.
		TEST_CASE("ProbeRelight: a reflection probe inside a relit volume dims with it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			PublishChrome(fixture);
			// The noon sky's radiance, from a render without probes, bakes the volumes (their coefficients at that sky).
			fixture.Submit(MakeChromeSnapshot(fixture, 70.0f, 30));
			const ProbeSH9 noonSky = ReadSkyRadiance(device, fixture.Renderer());
			const ProbeSH9 lamp = MakeLamp(glm::vec3(0.02f));
			const std::array<IrradianceProbeRelight, 8> blocks = MakeCornerBlocks(lamp, true);
			fixture.Assets().Publish(RelitVolumeHandle, MakeBakedVolume(4.0f, blocks, noonSky));
			fixture.Assets().Publish(OuterVolumeHandle, MakeBakedVolume(12.0f, MakeCornerBlocks(lamp, false), noonSky));
			const auto render = [&fixture, &device](float elevation, uint64_t frame, const glm::vec3& centre, bool outer)
			{
				RenderSnapshot snapshot = MakeChromeSnapshot(fixture, elevation, frame);
				snapshot.IrradianceVolumes.push_back(MakeVolumeItem(RelitVolumeHandle));
				if (outer)
					snapshot.IrradianceVolumes.push_back(MakeVolumeItem(OuterVolumeHandle, glm::vec3(0.0f, 0.0f, -3.0f), 12.0f, UUID(33)));
				snapshot.ReflectionProbes.push_back(MakeReflectionItem(ReflectionHandle, centre));
				fixture.Submit(snapshot);
				return MeanCentreGreen(device, fixture.Renderer());
			};
			// The volume's box is 4 m about (0, 0, -3): (1.5, 1.5, -4.5) is nearest probe 3, (-1.5, -1.5, -4.5) nearest the closed
			// probe 0; (0, 0, -10) is outside it (and outside the outer volume, 12 m about the same centre), its box still around
			// the quad.
			const glm::vec3 nearOpen(1.5f, 1.5f, -4.5f);
			const glm::vec3 nearClosed(-1.5f, -1.5f, -4.5f);
			const glm::vec3 outside(0.0f, 0.0f, -10.0f);
			const double noon = render(70.0f, 31, nearOpen, false);
			const double night = render(-30.0f, 32, nearOpen, false);
			const ProbeSH9 nightSky = ReadSkyRadiance(device, fixture.Renderer());
			const ProbeSH9 baked3 = RelightIrradianceSH9(lamp, blocks[3].Transfer, noonSky);
			const float expected = ComputeReflectionRelightScale(RelightIrradianceSH9(lamp, blocks[3].Transfer, nightSky)[0], baked3[0]);
			const double closedNight = render(-30.0f, 33, nearClosed, false);
			const double nestedNight = render(-30.0f, 34, nearOpen, true);
			const double kept = render(-30.0f, 35, outside, false);
			INFO("noon " << noon << ", night " << night << " (expected scale " << expected << "), nearest a closed probe at night " << closedNight
						 << ", inside the outer volume at night " << nestedNight << ", outside at night " << kept);
			CHECK(noon > 0.0);
			CHECK(expected < 0.5f);
			CHECK(night / noon == doctest::Approx(expected).epsilon(0.03).scale(0.0));
			CHECK(closedNight == doctest::Approx(noon).epsilon(0.01).scale(0.0));
			CHECK(nestedNight == doctest::Approx(night).epsilon(0.01).scale(0.0));
			CHECK(kept == doctest::Approx(noon).epsilon(0.01).scale(0.0));
			device.RunGarbageCollection();
		}

		// The W45.3 review's finding 3: the relit volume around a reflection probe's centre is behind the camera, out of the
		// view, while the probe's box holds the quad in front of it. The view keeps the volume for the reflection's relight, so
		// the reflection dims at night by the same ratio as with the volume in view, and the volume is relit off screen.
		TEST_CASE("ProbeRelight: a reflection probe keeps its relit scale while the volume around its centre is out of view" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			PublishChrome(fixture);
			fixture.Submit(MakeChromeSnapshot(fixture, 70.0f, 50));
			const ProbeSH9 noonSky = ReadSkyRadiance(device, fixture.Renderer());
			const ProbeSH9 lamp = MakeLamp(glm::vec3(0.02f));
			const std::array<IrradianceProbeRelight, 8> blocks = MakeCornerBlocks(lamp, true);
			fixture.Assets().Publish(RelitVolumeHandle, MakeBakedVolume(4.0f, blocks, noonSky));
			// The volume 4 m about (0, 0, 5), behind the camera at the origin looking down -Z; the probe's centre nearest its probe
			// 3, (1.5, 1.5, 3.5).
			const auto render = [&fixture, &device](float elevation, uint64_t frame)
			{
				RenderSnapshot snapshot = MakeChromeSnapshot(fixture, elevation, frame);
				snapshot.IrradianceVolumes.push_back(MakeVolumeItem(RelitVolumeHandle, glm::vec3(0.0f, 0.0f, 5.0f)));
				snapshot.ReflectionProbes.push_back(MakeReflectionItem(ReflectionHandle, glm::vec3(1.5f, 1.5f, 3.5f)));
				fixture.Submit(snapshot);
				return MeanCentreGreen(device, fixture.Renderer());
			};
			const double noon = render(70.0f, 51);
			const double night = render(-30.0f, 52);
			CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 1U);
			const ProbeSH9 nightSky = ReadSkyRadiance(device, fixture.Renderer());
			const ProbeSH9 baked3 = RelightIrradianceSH9(lamp, blocks[3].Transfer, noonSky);
			const float expected = ComputeReflectionRelightScale(RelightIrradianceSH9(lamp, blocks[3].Transfer, nightSky)[0], baked3[0]);
			INFO("noon " << noon << ", night " << night << " (expected scale " << expected << ")");
			CHECK(noon > 0.0);
			CHECK(expected < 0.5f);
			CHECK(night / noon == doctest::Approx(expected).epsilon(0.03).scale(0.0));
			device.RunGarbageCollection();
		}

		// The W45.3 review's finding 2: the view relights its volumes when the Sky's Intensity changes, which the sky's key leaves
		// out (SkyUpdates 0), and a view whose copy holds relit probes takes their bakes again once the Sky is gone. The cave's
		// volume and its black reflection probe, as the cave case: the quad shows its probes' diffuse light alone.
		TEST_CASE("ProbeRelight: a relit volume follows the Sky's Intensity and takes its bake again without the Sky" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			const ProbeSH9 lamp = MakeLamp(glm::vec3(0.005f));
			const IrradianceProbeRelight block{ .Local = lamp, .Transfer = MakeCaveTransfer() };
			fixture.Assets().Publish(RelitVolumeHandle, MakeVolume(lamp, block, true));
			fixture.Assets().Publish(BakedVolumeHandle, MakeVolume(lamp, block, false));
			fixture.Assets().Publish(ReflectionHandle, MakeUniformEnvironment(0.0));
			const auto render = [&fixture, &device](AssetHandle volume, float intensity, bool sky, uint64_t frame)
			{
				RenderSnapshot snapshot = MakeRelitSnapshot(fixture, 70.0f);
				snapshot.Sky.Intensity = intensity;
				snapshot.Sky.Enabled = sky;
				snapshot.IrradianceVolumes.push_back(MakeVolumeItem(volume));
				snapshot.ReflectionProbes.push_back(MakeReflectionItem(ReflectionHandle, glm::vec3(0.0f, 0.0f, -10.0f)));
				snapshot.FrameIndex = frame;
				fixture.Submit(snapshot);
				return MeanCentreGreen(device, fixture.Renderer());
			};
			// The probes' bake is the lamp's light alone (MakeVolume's coefficients).
			const double lampAlone = render(BakedVolumeHandle, 1.0f, true, 40);
			const double noon = render(RelitVolumeHandle, 1.0f, true, 41);
			CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 1U);
			CHECK(render(RelitVolumeHandle, 1.0f, true, 42) == noon);
			CHECK(fixture.Renderer().GetRenderStats().RelitIrradianceVolumes == 0U);
			// Half the Sky's Intensity: the sky's light is not regenerated, the volume is relit, and the quad's light from the
			// sky halves.
			const double half = render(RelitVolumeHandle, 0.5f, true, 43);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.SkyUpdates == 0U);
			CHECK(stats.RelitIrradianceVolumes == 1U);
			INFO("the lamp alone " << lampAlone << ", noon " << noon << ", at half Intensity " << half);
			CHECK(noon > 2.0 * lampAlone);
			// Relative tolerances (scale 0): doctest's default scale of 1 would accept any difference below the epsilon here.
			CHECK(half - lampAlone == doctest::Approx(0.5 * (noon - lampAlone)).epsilon(0.05).scale(0.0));
			// Without the Sky, the view's copy of the relit volume takes its bake again: the lamp's light, as the volume baked
			// without a relight block gives without the Sky.
			const double withoutSky = render(RelitVolumeHandle, 0.5f, false, 44);
			stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.RelitIrradianceVolumes == 0U);
			CHECK(stats.SkyUpdates == 0U);
			const double bakedWithoutSky = render(BakedVolumeHandle, 0.5f, false, 45);
			INFO("without the Sky " << withoutSky << ", the baked volume without the Sky " << bakedWithoutSky);
			CHECK(withoutSky == bakedWithoutSky);
			device.RunGarbageCollection();
		}
	}

}
