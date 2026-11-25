#include "TestsPCH.h"

#include "Engine/Renderer/SkyLighting.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/IEnvironmentBaker.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/EnvironmentBaker.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/PlanarReflections.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/ProbeCapture.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SkyAtmosphere.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"
#include "Support/BufferReadback.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"
#include "Support/SkyCopyProgram.h"
#include "Support/TextureCreationFails.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <numbers>
#include <span>
#include <string>
#include <vector>

// The sky's lighting on the GPU (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h).

namespace Engine {

	namespace {

		// The value element `element` of a SkyResults buffer holds in component `component` after CSTestCopyWrite with `seed`
		// (SkyConstants::Intensity).
		float GetSkyTestResult(float seed, uint32_t element, uint32_t component)
		{
			return seed + 4.0f * static_cast<float>(element) + static_cast<float>(component);
		}

		// A static constant buffer made as the scene renderer makes its per-view constants (SceneRenderer.cpp).
		nvrhi::BufferHandle CreateViewConstantBuffer(GraphicsDevice& device, size_t byteSize, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = byteSize;
			desc.isConstantBuffer = true;
			desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			desc.keepInitialState = true;
			desc.debugName = name;
			Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
			return std::move(*buffer);
		}

		nvrhi::BufferHandle CreateVectorBuffer(GraphicsDevice& device, uint32_t count, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = count * sizeof(glm::vec4);
			desc.structStride = sizeof(glm::vec4);
			desc.canHaveUAVs = true;
			desc.initialState = nvrhi::ResourceStates::UnorderedAccess;
			desc.keepInitialState = true;
			desc.debugName = name;
			Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
			return std::move(*buffer);
		}

		nvrhi::BufferHandle CreateReadbackBuffer(GraphicsDevice& device, uint32_t count, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = count * sizeof(glm::vec4);
			desc.cpuAccess = nvrhi::CpuAccessMode::Read;
			desc.initialState = nvrhi::ResourceStates::CopyDest;
			desc.keepInitialState = true;
			desc.debugName = name;
			Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
			return std::move(*buffer);
		}

		// What one frame's CPU writes put into the constant buffers: every member distinct, and different in each frame.
		struct SkyCopyFrame
		{
			float Seed = 0.0f;
			EnvironmentConstants Environment{};
			FogConstants Fog{};
			FogConstants MirrorFog{};
			ProbeConstants Probes{};
		};

		SkyCopyFrame MakeSkyCopyFrame(float base, float seed)
		{
			SkyCopyFrame frame;
			frame.Seed = seed;
			for (uint32_t index = 0; index < 9; ++index)
				frame.Environment.IrradianceSH9[index] = glm::vec4(base + static_cast<float>(index));
			frame.Environment.AmbientColor = glm::vec3(base + 20.0f, base + 21.0f, base + 22.0f);
			frame.Environment.Intensity = base + 23.0f;
			frame.Environment.RotationSin = base + 24.0f;
			frame.Environment.RotationCos = base + 25.0f;
			frame.Environment.SpecularMaxLod = base + 26.0f;
			frame.Environment.SkyboxLod = base + 27.0f;
			frame.Environment.HasEnvironment = 1;
			frame.Fog.Color = glm::vec3(base + 30.0f, base + 31.0f, base + 32.0f);
			frame.Fog.Density = base + 33.0f;
			frame.Fog.CameraPosition = glm::vec3(base + 34.0f, base + 35.0f, base + 36.0f);
			frame.Fog.StartDistance = base + 37.0f;
			frame.MirrorFog = frame.Fog;
			frame.MirrorFog.Density = base + 38.0f;
			frame.MirrorFog.CameraPosition = glm::vec3(base + 39.0f);
			for (uint32_t probe = 0; probe < MaxViewReflectionProbes; ++probe)
			{
				const float value = base + 40.0f + 8.0f * static_cast<float>(probe);
				frame.Probes.Reflections[probe].AxisScale = glm::vec4(value, value + 1.0f, value + 2.0f, value + 3.0f);
				frame.Probes.Reflections[probe].Lookup = glm::vec4(value + 4.0f, value + 5.0f, value + 6.0f, value + 7.0f);
			}
			frame.Probes.Counts = glm::uvec4(0, 3, 0, 0);
			return frame;
		}

		void CheckVector(const glm::vec4& actual, const glm::vec4& expected)
		{
			for (glm::length_t component = 0; component < 4; ++component)
			{
				CAPTURE(component);
				CHECK(actual[component] == expected[component]);
			}
		}

		// The texels of level `mip`, layer `layer` of an RGBA16F `texture`, as linear values.
		std::vector<glm::dvec4> ReadHalfTexels(GraphicsDevice& device, nvrhi::ITexture& texture, uint32_t mip = 0, uint32_t layer = 0)
		{
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(texture, mip, layer);
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

		std::vector<glm::vec4> ReadVectors(GraphicsDevice& device, nvrhi::IBuffer& buffer, uint32_t count)
		{
			const Result<std::vector<std::byte>> bytes = Test::ReadBufferBytes(device, buffer, count * sizeof(glm::vec4));
			REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
			std::vector<glm::vec4> vectors(count);
			std::memcpy(vectors.data(), bytes->data(), count * sizeof(glm::vec4));
			return vectors;
		}

		// The transmittance LUT's texel (x, y) on the CPU: Bruneton's mapping inverted at x / (width - 1), y / (height - 1), then
		// the transmittance to the top in double (the ray of a texel never meets the ground; the last column grazes the horizon,
		// so its cosine is raised past the ground's by 1e-9).
		glm::dvec3 ComputeTransmittanceTexel(const SkyAtmosphere& atmosphere, uint32_t x, uint32_t y)
		{
			const double h = std::sqrt(SkyTopRadius * SkyTopRadius - SkyBottomRadius * SkyBottomRadius);
			const double rho = h * (static_cast<double>(y) / (SkyTransmittanceLutHeight - 1));
			const double r = std::sqrt(rho * rho + SkyBottomRadius * SkyBottomRadius);
			const double dMin = SkyTopRadius - r;
			const double dMax = rho + h;
			const double d = dMin + (static_cast<double>(x) / (SkyTransmittanceLutWidth - 1)) * (dMax - dMin);
			double mu = d == 0.0 ? 1.0 : std::clamp((h * h - rho * rho - d * d) / (2.0 * r * d), -1.0, 1.0);
			const double horizon = -std::sqrt(std::max(1.0 - SkyBottomRadius * SkyBottomRadius / (r * r), 0.0));
			mu = std::max(mu, horizon + 1e-9);
			return ComputeSkyTransmittance(atmosphere, r, mu);
		}

		// CSMultipleScatteringLut's texel (x, y) in double, with the sun's transmittance exact rather than from the LUT.
		glm::dvec3 ComputeMultipleScatteringTexel(const SkyAtmosphere& atmosphere, uint32_t x, uint32_t y)
		{
			const double pi = std::numbers::pi;
			const double cosSun = static_cast<double>(x) / (SkyMultipleScatteringLutSize - 1) * 2.0 - 1.0;
			const double unitHeight = std::clamp(static_cast<double>(y) / (SkyMultipleScatteringLutSize - 1), 0.0005, 0.9995);
			const double r = SkyBottomRadius + unitHeight * (SkyTopRadius - SkyBottomRadius);
			const glm::dvec3 sun(std::sqrt(std::max(1.0 - cosSun * cosSun, 0.0)), cosSun, 0.0);
			const glm::dvec3 origin(0.0, r, 0.0);
			const auto scatteringAt = [&atmosphere](double height)
			{
				return atmosphere.RayleighScattering * std::exp(-height / SkyRayleighScaleHeight) + glm::dvec3(atmosphere.MieScattering * std::exp(-height / SkyMieScaleHeight));
			};
			glm::dvec3 second(0.0);
			glm::dvec3 transfer(0.0);
			constexpr uint32_t Directions = 8;
			constexpr uint32_t Steps = 20;
			for (uint32_t i = 0; i < Directions; ++i)
			{
				for (uint32_t j = 0; j < Directions; ++j)
				{
					const double cosTheta = 1.0 - 2.0 * (i + 0.5) / Directions;
					const double sinTheta = std::sqrt(std::max(1.0 - cosTheta * cosTheta, 0.0));
					const double phi = 2.0 * pi * (j + 0.5) / Directions;
					const glm::dvec3 direction(sinTheta * std::cos(phi), cosTheta, sinTheta * std::sin(phi));
					const double groundDiscriminant = r * r * (cosTheta * cosTheta - 1.0) + SkyBottomRadius * SkyBottomRadius;
					const bool ground = cosTheta < 0.0 && groundDiscriminant >= 0.0;
					const double distance = ground ? std::max(-r * cosTheta - std::sqrt(std::max(groundDiscriminant, 0.0)), 0.0)
												   : std::max(-r * cosTheta + std::sqrt(std::max(r * r * (cosTheta * cosTheta - 1.0) + SkyTopRadius * SkyTopRadius, 0.0)), 0.0);
					const double step = distance / Steps;
					glm::dvec3 throughput(1.0);
					for (uint32_t k = 0; k < Steps; ++k)
					{
						const glm::dvec3 position = origin + direction * ((k + 0.5) * step);
						const double radius = glm::length(position);
						const double height = radius - SkyBottomRadius;
						const glm::dvec3 scattering = scatteringAt(height);
						const glm::dvec3 extinction = ComputeSkyExtinction(atmosphere, height);
						const glm::dvec3 stepTransmittance(std::exp(-extinction.x * step), std::exp(-extinction.y * step), std::exp(-extinction.z * step));
						const glm::dvec3 sunTransmittance = ComputeSkyTransmittance(atmosphere, radius, glm::dot(position / radius, sun));
						const glm::dvec3 inScattered = scattering * sunTransmittance / (4.0 * pi);
						const glm::dvec3 safe = glm::max(extinction, glm::dvec3(1e-6));
						second += throughput * (inScattered - inScattered * stepTransmittance) / safe;
						transfer += throughput * (scattering - scattering * stepTransmittance) / safe;
						throughput *= stepTransmittance;
					}
					if (ground)
					{
						const glm::dvec3 position = origin + direction * distance;
						const glm::dvec3 normal = glm::normalize(position);
						const glm::dvec3 sunTransmittance = ComputeSkyTransmittance(atmosphere, glm::length(position), glm::dot(normal, sun));
						second += throughput * sunTransmittance * std::max(glm::dot(normal, sun), 0.0) * atmosphere.GroundAlbedo / pi;
					}
				}
			}
			second /= static_cast<double>(Directions * Directions);
			transfer /= static_cast<double>(Directions * Directions);
			return second / glm::max(glm::dvec3(1.0) - transfer, glm::dvec3(1e-3));
		}

		// A Sky lit by one sun `elevation` degrees above the horizon toward -Z, for the CPU-side constants of the pass tests.
		RenderSnapshot MakeSkySnapshot(float elevation)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
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

		// The sky's pass and resources on `gpu`'s device, with `constants` written.
		struct SkyPassSetup
		{
			Scope<SkyLightingPass> Pass{};
			SkyViewResources Resources{};
			PassBindingCache Bindings{};
		};

		SkyPassSetup MakeSkyPassSetup(Test::HeadlessGpuFixture& gpu)
		{
			SkyPassSetup setup;
			Result<Scope<SkyLightingPass>> pass = SkyLightingPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			setup.Pass = std::move(*pass);
			Result<SkyViewResources> resources = CreateSkyViewResources(gpu.GetDevice());
			REQUIRE_MESSAGE(resources.has_value(), resources.error().ToString());
			setup.Resources = std::move(*resources);
			return setup;
		}

		// Records `record` on a command list of its own, after writing `constants`, and waits for it.
		template<typename Record>
		void RunSkyCommands(GraphicsDevice& device, SkyPassSetup& setup, const SkyConstants& constants, const Record& record)
		{
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->writeBuffer(setup.Resources.Constants, &constants, sizeof(constants));
			record(**commandList);
			(*commandList)->close();
			WaitForSubmission(device, device.ExecuteCommandList(**commandList), "the sky test");
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SkyLighting: a buffer copied into a view's constant buffers after their CPU writes reaches the passes that read them"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Decision 3, "The copy": two frames in flight, each CPU-writing the view's static constant buffers as RecordViewSetup
			// does, then a compute writing a SkyResults buffer, RecordSkyResultCopies over three of the copies' places (the SH9,
			// the fog's colour of the view and of a mirror slot, three reflection probes' AxisScale.w), and a compute reading the
			// constant buffers back. The copied bytes, their untouched neighbours and the second frame are checked; the fixture
			// fails the case on any validation or synchronization validation message (Test.py runs it at API 1.4 and 1.3).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<ComputePipeline> write = gpu.GetPipelines().CreateComputePipeline({ .Layout = Test::MakeSkyCopyWriteLayoutDescription() });
			REQUIRE_MESSAGE(write.has_value(), write.error().ToString());
			Result<ComputePipeline> read = gpu.GetPipelines().CreateComputePipeline({ .Layout = Test::MakeSkyCopyReadLayoutDescription() });
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			REQUIRE(write->BindingLayouts.size() == 1);
			REQUIRE(read->BindingLayouts.size() == 1);

			const nvrhi::BufferHandle sky = CreateViewConstantBuffer(device, sizeof(SkyConstants), "SkyCopy.Sky");
			const nvrhi::BufferHandle environment = CreateViewConstantBuffer(device, sizeof(EnvironmentConstants), "SkyCopy.Environment");
			const nvrhi::BufferHandle fog = CreateViewConstantBuffer(device, sizeof(FogConstants), "SkyCopy.Fog");
			const nvrhi::BufferHandle mirrorFog = CreateViewConstantBuffer(device, sizeof(FogConstants), "SkyCopy.MirrorFog");
			const nvrhi::BufferHandle probes = CreateViewConstantBuffer(device, sizeof(ProbeConstants), "SkyCopy.Probes");
			const nvrhi::BufferHandle results = CreateVectorBuffer(device, SkyResultsCount, "SkyCopy.Results");

			Result<nvrhi::BindingSetHandle> writeSet = device.CreateBindingSet(
				{ .bindings = { nvrhi::BindingSetItem::ConstantBuffer(1, sky), nvrhi::BindingSetItem::StructuredBuffer_UAV(6, results),
					  nvrhi::BindingSetItem::PushConstants(0, sizeof(SkyDispatchConstants)) } },
				*write->BindingLayouts[0]);
			REQUIRE_MESSAGE(writeSet.has_value(), writeSet.error().ToString());

			const std::array<SkyCopyFrame, 2> frames = { MakeSkyCopyFrame(1000.0f, 100.0f), MakeSkyCopyFrame(2000.0f, 200.0f) };
			constexpr uint32_t CopiedReflectionProbes = 3;
			// Per frame, the view's read (its fog) and the mirror slot's (its fog), each into its own output and readback.
			std::array<std::array<nvrhi::BufferHandle, 2>, 2> outputs;
			std::array<std::array<nvrhi::BufferHandle, 2>, 2> readbacks;
			std::array<std::array<nvrhi::BindingSetHandle, 2>, 2> readSets;
			std::vector<nvrhi::CommandListHandle> commandLists;
			uint64_t lastSubmission = 0;
			for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
			{
				const SkyCopyFrame& frame = frames[frameIndex];
				for (size_t view = 0; view < 2; ++view)
				{
					outputs[frameIndex][view] = CreateVectorBuffer(device, Test::SkyCopyReadbackCount, "SkyCopy.Output");
					readbacks[frameIndex][view] = CreateReadbackBuffer(device, Test::SkyCopyReadbackCount, "SkyCopy.Readback");
					Result<nvrhi::BindingSetHandle> set = device.CreateBindingSet({ .bindings = { nvrhi::BindingSetItem::ConstantBuffer(2, environment),
																						nvrhi::BindingSetItem::ConstantBuffer(4, probes),
																						nvrhi::BindingSetItem::ConstantBuffer(5, view == 0 ? fog : mirrorFog),
																						nvrhi::BindingSetItem::StructuredBuffer_UAV(7, outputs[frameIndex][view]), nvrhi::BindingSetItem::PushConstants(0, sizeof(SkyDispatchConstants)) } },
						*read->BindingLayouts[0]);
					REQUIRE_MESSAGE(set.has_value(), set.error().ToString());
					readSets[frameIndex][view] = std::move(*set);
				}

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				nvrhi::ICommandList& list = **commandList;
				list.open();
				// RecordViewSetup's CPU writes.
				SkyConstants constants{};
				constants.Intensity = frame.Seed;
				list.writeBuffer(sky, &constants, sizeof(constants));
				list.writeBuffer(environment, &frame.Environment, sizeof(frame.Environment));
				list.writeBuffer(fog, &frame.Fog, sizeof(frame.Fog));
				list.writeBuffer(mirrorFog, &frame.MirrorFog, sizeof(frame.MirrorFog));
				list.writeBuffer(probes, &frame.Probes, sizeof(frame.Probes));
				// The sky's compute, then the copies, then the passes that read the constants.
				nvrhi::ComputeState writeState;
				writeState.pipeline = write->Pipeline;
				writeState.bindings = { *writeSet };
				list.setComputeState(writeState);
				const SkyDispatchConstants unused{};
				list.setPushConstants(&unused, sizeof(unused));
				list.dispatch(1);
				const std::array<nvrhi::IBuffer*, 2> fogs = { fog.Get(), mirrorFog.Get() };
				RecordSkyResultCopies(list, results, environment, fogs, probes, CopiedReflectionProbes);
				for (size_t view = 0; view < 2; ++view)
				{
					nvrhi::ComputeState readState;
					readState.pipeline = read->Pipeline;
					readState.bindings = { readSets[frameIndex][view] };
					list.setComputeState(readState);
					list.setPushConstants(&unused, sizeof(unused));
					list.dispatch(1);
					list.copyBuffer(readbacks[frameIndex][view], 0, outputs[frameIndex][view], 0, Test::SkyCopyReadbackCount * sizeof(glm::vec4));
				}
				list.close();
				// Both frames are submitted before either is waited for, as a renderer's frames in flight are.
				lastSubmission = device.ExecuteCommandList(list);
				commandLists.push_back(std::move(*commandList));
			}
			WaitForSubmission(device, lastSubmission, "the sky copy test");

			for (size_t frameIndex = 0; frameIndex < frames.size(); ++frameIndex)
			{
				CAPTURE(frameIndex);
				const SkyCopyFrame& frame = frames[frameIndex];
				for (size_t view = 0; view < 2; ++view)
				{
					CAPTURE(view);
					std::array<glm::vec4, Test::SkyCopyReadbackCount> seen{};
					const void* mapped = device.GetNvrhiDevice()->mapBuffer(readbacks[frameIndex][view], nvrhi::CpuAccessMode::Read);
					REQUIRE(mapped != nullptr);
					std::memcpy(seen.data(), mapped, sizeof(seen));
					device.GetNvrhiDevice()->unmapBuffer(readbacks[frameIndex][view]);

					// The SH9: the sky's values, copied over the CPU's.
					for (uint32_t index = 0; index < 9; ++index)
					{
						CAPTURE(index);
						CheckVector(seen[index], glm::vec4(GetSkyTestResult(frame.Seed, SkyResultsIrradiance + index, 0), GetSkyTestResult(frame.Seed, SkyResultsIrradiance + index, 1), GetSkyTestResult(frame.Seed, SkyResultsIrradiance + index, 2), GetSkyTestResult(frame.Seed, SkyResultsIrradiance + index, 3)));
					}
					// EnvironmentConstants' other members, as the CPU wrote them.
					CheckVector(seen[9], glm::vec4(frame.Environment.AmbientColor, frame.Environment.Intensity));
					CheckVector(seen[10], glm::vec4(frame.Environment.RotationSin, frame.Environment.RotationCos, frame.Environment.SpecularMaxLod, frame.Environment.SkyboxLod));
					uint32_t hasEnvironment = 0;
					std::memcpy(&hasEnvironment, &seen[11].x, sizeof(hasEnvironment));
					CHECK(hasEnvironment == 1U);
					// The fog's colour copied, its Density and the rest as the CPU wrote them (the view's and the mirror slot's own).
					const FogConstants& viewFog = view == 0 ? frame.Fog : frame.MirrorFog;
					CheckVector(seen[12], glm::vec4(GetSkyTestResult(frame.Seed, SkyResultsFogColor, 0), GetSkyTestResult(frame.Seed, SkyResultsFogColor, 1), GetSkyTestResult(frame.Seed, SkyResultsFogColor, 2), viewFog.Density));
					CheckVector(seen[13], glm::vec4(viewFog.CameraPosition, viewFog.StartDistance));
					// Each copied reflection probe's AxisScale.w; the other probes' and every Lookup as the CPU wrote them.
					for (uint32_t probe = 0; probe < MaxViewReflectionProbes; ++probe)
					{
						CAPTURE(probe);
						const ProbeReflectionConstants& written = frame.Probes.Reflections[probe];
						const float scale = probe < CopiedReflectionProbes ? GetSkyTestResult(frame.Seed, SkyResultsReflectionScales + probe, 0) : written.AxisScale.w;
						CheckVector(seen[14 + 2 * probe], glm::vec4(glm::vec3(written.AxisScale), scale));
						CheckVector(seen[15 + 2 * probe], written.Lookup);
					}
				}
			}
		}

		TEST_CASE("SkyLighting: the transmittance and multiple-scattering LUTs match their CPU reference" * doctest::test_suite(Test::GpuSuite))
		{
			// The LUTs in float on the GPU against their definitions in double (Renderer/SkyAtmosphere.h), on a hazy, slightly
			// overcast atmosphere: every transmittance texel, and every fourth multiple-scattering texel in each direction (the GPU
			// reads the sun's transmittance from the LUT, the reference computes it).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			SkyPassSetup setup = MakeSkyPassSetup(gpu);
			RenderSnapshot snapshot = MakeSkySnapshot(30.0f);
			snapshot.Sky.Haze = 2.0f;
			snapshot.Sky.Overcast = 0.1f;
			snapshot.Sky.GroundAlbedo = glm::vec3(0.4f, 0.3f, 0.2f);
			const SkyConstants constants = MakeSkyConstants(snapshot.Sky, snapshot.Lights, snapshot.Camera, snapshot.Fog);
			RunSkyCommands(gpu.GetDevice(), setup, constants, [&setup](nvrhi::ICommandList& commandList)
			{
				const Result<uint32_t> recorded = setup.Pass->RecordLuts(commandList, setup.Bindings, setup.Resources, true, 0.001f);
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				CHECK(*recorded == 3U);
			});
			const SkyAtmosphere atmosphere = MakeSkyAtmosphere(2.0f, 0.1f, glm::vec3(0.4f, 0.3f, 0.2f));
			const std::vector<glm::dvec4> transmittance = ReadHalfTexels(gpu.GetDevice(), *setup.Resources.Transmittance);
			REQUIRE(transmittance.size() == size_t{ SkyTransmittanceLutWidth } * SkyTransmittanceLutHeight);
			double worst = 0.0;
			for (uint32_t y = 0; y < SkyTransmittanceLutHeight; ++y)
				for (uint32_t x = 0; x < SkyTransmittanceLutWidth; ++x)
				{
					const glm::dvec3 expected = ComputeTransmittanceTexel(atmosphere, x, y);
					const glm::dvec3 actual(transmittance[y * SkyTransmittanceLutWidth + x]);
					for (glm::length_t channel = 0; channel < 3; ++channel)
						worst = std::max(worst, std::abs(actual[channel] - expected[channel]) / std::max(expected[channel], 1e-3));
				}
			CHECK(worst < 0.01);
			const std::vector<glm::dvec4> multiple = ReadHalfTexels(gpu.GetDevice(), *setup.Resources.MultipleScattering);
			REQUIRE(multiple.size() == size_t{ SkyMultipleScatteringLutSize } * SkyMultipleScatteringLutSize);
			double worstMultiple = 0.0;
			for (uint32_t y = 0; y < SkyMultipleScatteringLutSize; y += 4)
				for (uint32_t x = 0; x < SkyMultipleScatteringLutSize; x += 4)
				{
					const glm::dvec3 expected = ComputeMultipleScatteringTexel(atmosphere, x, y);
					const glm::dvec3 actual(multiple[y * SkyMultipleScatteringLutSize + x]);
					for (glm::length_t channel = 0; channel < 3; ++channel)
						worstMultiple = std::max(worstMultiple, std::abs(actual[channel] - expected[channel]) / std::max(expected[channel], 1e-4));
				}
			CHECK(worstMultiple < 0.05);
		}

		TEST_CASE("SkyLighting: the run-time prefilter of a cube matches the environment bake's" * doctest::test_suite(Test::GpuSuite))
		{
			// The environment bake of a 512 x 256 gradient with a bright spot, at the sky cube's size and sample counts, then its
			// skybox's level 0 as the sky cube's: the sky's RecordCubeLighting reuses the bake's four entries, so its specular cube
			// holds the bake's bytes, and its finalized SH9 the bake's coefficients.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<EnvironmentBaker>> baker = EnvironmentBaker::Create(device, gpu.GetPipelines());
			REQUIRE_MESSAGE(baker.has_value(), baker.error().ToString());
			EnvironmentBakeInput input;
			input.Width = 512;
			input.Height = 256;
			input.Texels.resize(static_cast<size_t>(input.Width) * input.Height * 3);
			for (uint32_t y = 0; y < input.Height; ++y)
				for (uint32_t x = 0; x < input.Width; ++x)
				{
					const size_t texel = (static_cast<size_t>(y) * input.Width + x) * 3;
					const float spot = (x > 100 && x < 110 && y > 60 && y < 70) ? 40.0f : 0.0f;
					input.Texels[texel + 0] = 0.2f + 0.6f * static_cast<float>(y) / input.Height + spot;
					input.Texels[texel + 1] = 0.3f + 0.2f * static_cast<float>(x) / input.Width + spot;
					input.Texels[texel + 2] = 0.9f - 0.5f * static_cast<float>(y) / input.Height + spot;
				}
			const Result<EnvironmentData> baked = (*baker)->BakeWithOptions(input, { .SkyboxFaceSize = SkyCubeSize, .MinSamples = SkySpecularMinSamples, .MaxSamples = SkySpecularMaxSamples });
			REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());

			SkyPassSetup setup = MakeSkyPassSetup(gpu);
			SkyConstants constants{};
			constants.Intensity = 1.0f;
			RunSkyCommands(device, setup, constants, [&setup, &baked](nvrhi::ICommandList& commandList)
			{
				const size_t faceBytes = static_cast<size_t>(SkyCubeSize) * SkyCubeSize * CubeMapData::BytesPerTexel;
				for (uint32_t face = 0; face < 6; ++face)
					commandList.writeTexture(setup.Resources.Cube, face, 0, baked->Skybox.Texels.data() + face * faceBytes, static_cast<size_t>(SkyCubeSize) * CubeMapData::BytesPerTexel);
				const Result<uint32_t> lighting = setup.Pass->RecordCubeLighting(commandList, setup.Bindings, setup.Resources);
				REQUIRE_MESSAGE(lighting.has_value(), lighting.error().ToString());
				const Status finalized = setup.Pass->RecordFinalize(commandList, setup.Bindings, setup.Resources);
				REQUIRE_MESSAGE(finalized.has_value(), finalized.error().ToString());
			});
			size_t offset = 0;
			for (uint32_t mip = 0; mip < EnvironmentData::SpecularMipCount; ++mip)
			{
				const uint32_t size = SkyCubeSize >> mip;
				const size_t faceBytes = static_cast<size_t>(size) * size * CubeMapData::BytesPerTexel;
				for (uint32_t face = 0; face < 6; ++face)
				{
					CAPTURE(mip);
					CAPTURE(face);
					Readback readback(device);
					const Result<Image> image = readback.ReadTexture(*setup.Resources.Specular, mip, face);
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					REQUIRE(image->Pixels.size() == faceBytes);
					CHECK(std::memcmp(image->Pixels.data(), baked->Specular.Texels.data() + offset, faceBytes) == 0);
					offset += faceBytes;
				}
			}
			const std::vector<glm::vec4> results = ReadVectors(device, *setup.Resources.Results, SkyResultsCount);
			for (uint32_t index = 0; index < 9; ++index)
			{
				CAPTURE(index);
				for (glm::length_t channel = 0; channel < 3; ++channel)
					CHECK(results[SkyResultsIrradiance + index][channel] == doctest::Approx(baked->IrradianceSH9[index][channel]).epsilon(1e-3).scale(1.0));
			}
		}

		TEST_CASE("SkyLighting: a constant sky gives a constant irradiance" * doctest::test_suite(Test::GpuSuite))
		{
			// The furnace: a cube of a uniform radiance of 2 prefilters to 2 in every mip and projects to the SH9 of a constant
			// irradiance (the constant term 2 / Y00, the others zero), times Intensity; the fog's colour takes its tint.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			SkyPassSetup setup = MakeSkyPassSetup(gpu);
			SkyConstants constants{};
			constants.Intensity = 1.5f;
			constants.FogColor = glm::vec3(0.5f, 0.25f, 1.0f);
			constants.FogAmbientTint = 1.0f;
			const std::vector<uint16_t> face(static_cast<size_t>(SkyCubeSize) * SkyCubeSize * 4, static_cast<uint16_t>(glm::packHalf1x16(2.0f)));
			RunSkyCommands(device, setup, constants, [&setup, &face](nvrhi::ICommandList& commandList)
			{
				for (uint32_t layer = 0; layer < 6; ++layer)
					commandList.writeTexture(setup.Resources.Cube, layer, 0, face.data(), static_cast<size_t>(SkyCubeSize) * 4 * sizeof(uint16_t));
				const Result<uint32_t> lighting = setup.Pass->RecordCubeLighting(commandList, setup.Bindings, setup.Resources);
				REQUIRE_MESSAGE(lighting.has_value(), lighting.error().ToString());
				const Status finalized = setup.Pass->RecordFinalize(commandList, setup.Bindings, setup.Resources);
				REQUIRE_MESSAGE(finalized.has_value(), finalized.error().ToString());
			});
			for (uint32_t mip = 0; mip < EnvironmentData::SpecularMipCount; ++mip)
			{
				CAPTURE(mip);
				for (const glm::dvec4& texel : ReadHalfTexels(device, *setup.Resources.Specular, mip, 3))
					CHECK(texel.r == doctest::Approx(2.0).epsilon(0.01));
			}
			const std::vector<glm::vec4> results = ReadVectors(device, *setup.Resources.Results, SkyResultsCount);
			const double constantTerm = 2.0 * 1.5 / 0.282094791773878;
			CHECK(results[SkyResultsIrradiance].r == doctest::Approx(constantTerm).epsilon(0.002));
			for (uint32_t index = 1; index < 9; ++index)
			{
				CAPTURE(index);
				CHECK(std::abs(results[SkyResultsIrradiance + index].g) < 0.002 * constantTerm);
			}
			// The fog's colour: Color times the irradiance's mean (3), clamped at nothing.
			CHECK(results[SkyResultsFogColor].r == doctest::Approx(0.5 * 3.0).epsilon(0.002));
			CHECK(results[SkyResultsFogColor].b == doctest::Approx(1.0 * 3.0).epsilon(0.002));
		}

		// The fixture's snapshot of its quad 3 m ahead, with a Sky lit by a sun `elevation` degrees up toward -Z (behind the quad,
		// whose face toward the camera faces +Z, so it is lit by the sky's image-based light alone), clearing to the sky.
		RenderSnapshot MakeSkyViewSnapshot(Test::SceneRendererIntegrationFixture & fixture, float elevation)
		{
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Camera.ClearToSkybox = true;
			snapshot.Camera.FarClip = 1000.0f;
			const RenderSnapshot sky = MakeSkySnapshot(elevation);
			snapshot.Lights = sky.Lights;
			snapshot.Sky = sky.Sky;
			ResolveSkyLights(snapshot);
			return snapshot;
		}

		// The mean linear radiance of SceneColor's pixels inside (`inside`) or outside the centre square of half its size.
		glm::dvec3 MeanSceneColor(const std::vector<glm::dvec4>& texels, uint32_t width, uint32_t height, bool inside)
		{
			glm::dvec3 sum(0.0);
			uint32_t count = 0;
			for (uint32_t y = 0; y < height; ++y)
				for (uint32_t x = 0; x < width; ++x)
				{
					const bool centre = x >= width * 3 / 8 && x < width * 5 / 8 && y >= height * 3 / 8 && y < height * 5 / 8;
					const bool corner = y < height / 8;
					if ((inside && centre) || (!inside && corner))
					{
						sum += glm::dvec3(texels[static_cast<size_t>(y) * width + x]);
						++count;
					}
				}
			return count > 0 ? sum / static_cast<double>(count) : sum;
		}

		TEST_CASE("SkyLighting: the view's ambient light, reflections and fog colour follow the sun" * doctest::test_suite(Test::GpuSuite))
		{
			// One scene at noon and at dusk, the quad lit by the sky alone: its image-based light (the diffuse SH9 and the specular
			// cube's reflection, both the sky's), the sky above it and the fog's colour (a dense fog tinted by the sky's ambient light
			// over the quad) all dim, and the sky reddens, as the sun sets. The W45.2 review's finding 4: a metallic, smooth quad,
			// whose colour is the sky's specular cube alone, shows the reflections follow it too.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			constexpr AssetHandle ChromeMaterial{ 0x9f46 };
			auto chrome = CreateRef<MaterialData>();
			chrome->BaseColor = glm::vec4(1.0f);
			chrome->Metallic = 1.0f;
			chrome->Roughness = 0.1f;
			fixture.Assets().Publish(ChromeMaterial, chrome);
			const auto render = [&gpu, &fixture](const RenderSnapshot& snapshot)
			{
				fixture.Submit(snapshot);
				return ReadHalfTexels(gpu.GetDevice(), *fixture.Renderer().GetSceneColorTexture());
			};
			struct Measured
			{
				glm::dvec3 Ambient{};
				glm::dvec3 Reflection{};
				glm::dvec3 Sky{};
				glm::dvec3 Fogged{};
			};
			const auto measure = [&fixture, &render, ChromeMaterial](float elevation)
			{
				Measured measured;
				RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, elevation);
				std::vector<glm::dvec4> texels = render(snapshot);
				measured.Ambient = MeanSceneColor(texels, 32, 32, true);
				measured.Sky = MeanSceneColor(texels, 32, 32, false);
				CHECK(fixture.Renderer().GetRenderStats().SkyUpdates == 1U);
				RenderSnapshot chromed = snapshot;
				chromed.Meshes[0].Materials = { ChromeMaterial };
				measured.Reflection = MeanSceneColor(render(chromed), 32, 32, true);
				snapshot.Fog = RenderFog{ .Enabled = true, .Color = glm::vec3(1.0f), .AmbientTint = 1.0f, .Density = 10.0f, .MaxOpacity = 1.0f, .FogSkybox = false };
				texels = render(snapshot);
				measured.Fogged = MeanSceneColor(texels, 32, 32, true);
				return measured;
			};
			const Measured noon = measure(80.0f);
			const Measured dusk = measure(2.0f);
			INFO("noon ambient " << noon.Ambient.r << " " << noon.Ambient.b << ", dusk " << dusk.Ambient.r << " " << dusk.Ambient.b);
			INFO("noon reflection " << noon.Reflection.r << " " << noon.Reflection.b << ", dusk " << dusk.Reflection.r << " " << dusk.Reflection.b);
			CHECK(noon.Ambient.g > 0.0);
			CHECK(noon.Ambient.g > 2.0 * dusk.Ambient.g);
			CHECK(noon.Reflection.g > 0.0);
			CHECK(noon.Reflection.g > 2.0 * dusk.Reflection.g);
			CHECK(noon.Sky.g > 2.0 * dusk.Sky.g);
			CHECK(noon.Fogged.g > 2.0 * dusk.Fogged.g);
			// At dusk the light, and its reflection, are redder relative to their blue than at noon.
			CHECK(dusk.Sky.r / dusk.Sky.b > noon.Sky.r / noon.Sky.b);
			CHECK(dusk.Reflection.r / dusk.Reflection.b > noon.Reflection.r / noon.Reflection.b);
		}

		TEST_CASE("SkyLighting: the sky's lighting is regenerated only when its key changes" * doctest::test_suite(Test::GpuSuite))
		{
			// RenderStats::SkyUpdates: 1 on the view's first render with a Sky, 0 for the same sky, a moved camera or the clouds'
			// drift within a SkyCloudLightingStep, 1 for a drift across a step and a turned Sun. The W45.2 review's finding 2: the
			// key holds only what the update reads, so 0 for a new Intensity (which the finalize applies every frame: the view's
			// image still changes), new stars or a new disc, a turned Moon without a light, and a cloudless sky's drift across a step.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 16, 16);
			RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, 30.0f);
			snapshot.Sky.Clouds = 0.5f;
			snapshot.Sky.SimulationTime = 10.2;
			const auto updates = [&fixture](const RenderSnapshot& rendered)
			{
				fixture.Submit(rendered);
				return fixture.Renderer().GetRenderStats().SkyUpdates;
			};
			CHECK(updates(snapshot) == 1U);
			CHECK(updates(snapshot) == 0U);
			snapshot.Camera.View = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -2.0f, 0.0f));
			snapshot.Camera.Position = glm::vec3(0.0f, 2.0f, 0.0f);
			CHECK(updates(snapshot) == 0U);
			snapshot.Sky.SimulationTime = 10.9;
			CHECK(updates(snapshot) == 0U);
			snapshot.Sky.SimulationTime = 11.05;
			CHECK(updates(snapshot) == 1U);
			snapshot.Lights[0].Direction = glm::normalize(glm::vec3(0.3f, -0.6f, 0.4f));
			CHECK(updates(snapshot) == 1U);
			CHECK(updates(snapshot) == 0U);
			const std::vector<glm::dvec4> before = ReadHalfTexels(gpu.GetDevice(), *fixture.Renderer().GetSceneColorTexture());
			snapshot.Sky.Intensity = 2.0f;
			CHECK(updates(snapshot) == 0U);
			CHECK(ReadHalfTexels(gpu.GetDevice(), *fixture.Renderer().GetSceneColorTexture()) != before);
			snapshot.Sky.Stars = 5.0f;
			snapshot.Sky.SunDisc = 3.0f;
			snapshot.Sky.MoonSize = 4.0f;
			CHECK(updates(snapshot) == 0U);
			// A Moon without a light scatters nothing, wherever it stands.
			snapshot.Sky.HasMoon = true;
			snapshot.Sky.MoonLight.reset();
			snapshot.Sky.MoonDirection = glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f));
			CHECK(updates(snapshot) == 0U);
			snapshot.Sky.MoonDirection = glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f));
			CHECK(updates(snapshot) == 0U);
			// Without clouds, their drift changes nothing.
			snapshot.Sky.Clouds = 0.0f;
			CHECK(updates(snapshot) == 1U);
			snapshot.Sky.SimulationTime = 12.05;
			CHECK(updates(snapshot) == 0U);
			// The passes' rows: SkyLuts and SkyIbl every frame (the finalize and the copies), the Sky pass where it draws.
			const RenderStats& stats = fixture.Renderer().GetRenderStats();
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SkyLuts") != nullptr);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SkyIbl") != nullptr);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "Sky") != nullptr);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "Skybox") == nullptr);
			// Without a Sky, none of them.
			snapshot.Sky.Enabled = false;
			fixture.Submit(snapshot);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "SkyLuts") == nullptr);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "Sky") == nullptr);
		}

		// The W45.2 review's finding 1: a shader reload (SceneRendererPipelines::CreatePipelines) replaces the pipelines that drew the
		// sky's LUTs and its image-based light, so the next render regenerates both, as the shadow cache's maps are redrawn.
		TEST_CASE("SkyLighting: the sky's lighting is regenerated after the pipelines are created again" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 16, 16);
			const RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, 30.0f);
			const auto render = [&fixture, &snapshot]()
			{
				fixture.Submit(snapshot);
				return fixture.Renderer().GetRenderStats();
			};
			CHECK(render().SkyUpdates == 1U);
			const RenderStats still = render();
			CHECK(still.SkyUpdates == 0U);
			const RenderPassStats* stillLuts = Test::SceneRendererIntegrationFixture::FindPass(still, "SkyLuts");
			REQUIRE(stillLuts != nullptr);
			CHECK(stillLuts->Dispatches == 1U);
			gpu.GetDevice().WaitForIdle();
			const Status reloaded = fixture.Pipelines().CreatePipelines();
			REQUIRE_MESSAGE(reloaded.has_value(), reloaded.error().ToString());
			gpu.GetDevice().RunGarbageCollection();
			const RenderStats reloadedStats = render();
			CHECK(reloadedStats.SkyUpdates == 1U);
			const RenderPassStats* luts = Test::SceneRendererIntegrationFixture::FindPass(reloadedStats, "SkyLuts");
			REQUIRE(luts != nullptr);
			CHECK(luts->Dispatches == 3U);
		}

		// The review of W45.1 (finding 2): the renderer gives the cascades to a Sky's Sun while it is above the horizon and to its
		// Moon light below it (OrderShadowCandidates in Prepare), whatever the lights' order. A wall facing the camera, an
		// occluder 1 m before it, and two shadow-casting directional lights of equal light, the Moon first in the list: the
		// light that owns the cascades darkens the wall behind the occluder along its direction, the other lights it unshadowed.
		TEST_CASE("SceneRenderer: with a Sky, the cascades' shadow follows its Sun by day and its Moon by night" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			constexpr uint32_t Size = 64;
			Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.Environment.FallbackColor = glm::vec3(0.0f);
			snapshot.Meshes[0].World = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -4.0f)), glm::vec3(10.0f));
			MeshDrawItem occluder = snapshot.Meshes[0];
			occluder.World = glm::scale(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -3.0f)), glm::vec3(0.4f));
			occluder.Entity = UUID(901);
			occluder.PickId = 0;
			snapshot.Meshes.push_back(occluder);
			const auto makeLight = [](const glm::vec3& toLight, uint64_t entity)
			{
				return LightData{ .Type = RenderLightType::Directional,
					.Color = glm::vec3(1.0f),
					.Intensity = 1.0f,
					.Direction = -glm::normalize(toLight),
					.LightAngle = 0.5f,
					.CastShadows = true,
					.Entity = UUID(entity),
					.ShadowDistance = 20.0f,
					.CascadeCount = 4,
					.DepthBias = 0.5f,
					.NormalBias = 0.5f };
			};
			// The Moon light first, then the Sun: snapshot order would give the cascades to the Moon.
			snapshot.Lights = { makeLight(glm::vec3(-1.0f, 0.5f, 1.0f), 81), makeLight(glm::vec3(1.0f, 0.5f, 1.0f), 82) };
			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = UUID(82);
			snapshot.Sky.Moon = UUID(81);
			snapshot.Sky.DriveLights = false;
			snapshot.Sky.Intensity = 0.0f; // the lights alone light the wall
			ResolveSkyLights(snapshot);
			REQUIRE(snapshot.Sky.SunLight == std::optional<uint32_t>(1));
			REQUIRE(snapshot.Sky.MoonLight == std::optional<uint32_t>(0));

			// The mean luminance of the 3 x 3 pixels about the wall's point `world` (at z = -4).
			const auto luminance = [&snapshot](const std::vector<glm::dvec4>& texels, const glm::vec3& world)
			{
				const glm::vec4 clip = snapshot.Camera.Projection * snapshot.Camera.View * glm::vec4(world, 1.0f);
				const int x = static_cast<int>((clip.x / clip.w + 1.0f) * 0.5f * static_cast<float>(Size));
				const int y = static_cast<int>((1.0f - clip.y / clip.w) * 0.5f * static_cast<float>(Size));
				double sum = 0.0;
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
						sum += glm::dot(glm::dvec3(texels[static_cast<size_t>(y + dy) * Size + static_cast<size_t>(x + dx)]), glm::dvec3(0.2126, 0.7152, 0.0722));
				return sum / 9.0;
			};
			const auto render = [&gpu, &fixture](const RenderSnapshot& rendered)
			{
				fixture.Submit(rendered);
				return ReadHalfTexels(gpu.GetDevice(), *fixture.Renderer().GetSceneColorTexture());
			};
			// The occluder's shadow along each light: the occluder's centre (0, 0, -3) moved back to the wall along it.
			const glm::vec3 behindForSun(-1.0f, -0.5f, -4.0f);
			const glm::vec3 behindForMoon(1.0f, -0.5f, -4.0f);

			// By day the Sun casts the shadow: behind the occluder along the Sun only the Moon lights the wall.
			std::vector<glm::dvec4> texels = render(snapshot);
			const double daySun = luminance(texels, behindForSun);
			const double dayMoon = luminance(texels, behindForMoon);
			INFO("day: behind along the Sun " << daySun << ", along the Moon " << dayMoon);
			CHECK(daySun > 0.0);
			CHECK(daySun < 0.75 * dayMoon);

			// By night (the Sun below the horizon, still lighting the wall) the Moon casts it.
			snapshot.Lights[1].Direction = -glm::normalize(glm::vec3(1.0f, -0.5f, 1.0f));
			texels = render(snapshot);
			const double nightMoon = luminance(texels, behindForMoon);
			const double nightOpen = luminance(texels, behindForSun + glm::vec3(0.0f, 1.0f, 0.0f));
			INFO("night: behind along the Moon " << nightMoon << ", open wall " << nightOpen);
			CHECK(nightMoon > 0.0);
			CHECK(nightMoon < 0.75 * nightOpen);
		}

		// A mirror (M42's planar reflection) and a probe capture draw the Sky as a view does: the glass shows the sky behind the
		// camera, what the camera mirrored in the glass's plane renders (the mirror view draws the Sky in the skybox's place,
		// sampling the main view's sky-view LUT by world direction), and a capture's upward face holds the day's blue sky.
		TEST_CASE("SkyPass: a mirror view and a probe capture draw the sky" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			constexpr uint32_t Size = 64;
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
				constexpr AssetHandle MirrorMaterial{ 0x9f45 };
				auto mirror = CreateRef<MaterialData>();
				mirror->BaseColor = glm::vec4(1.0f);
				mirror->Metallic = 1.0f;
				mirror->Roughness = 0.0f;
				fixture.Assets().Publish(MirrorMaterial, mirror);
				// The sun 30 degrees up ahead of the camera; the glass faces the camera, so it shows the sky behind it.
				RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, 30.0f);
				snapshot.Post.SsrEnabled = false;
				snapshot.Meshes[0].Materials = { MirrorMaterial };
				snapshot.Meshes[0].CastShadows = false;
				snapshot.PlanarReflections.push_back(
					PlanarReflectionItem{ .World = snapshot.Meshes[0].World, .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .ResolutionScale = 1.0f, .Entity = snapshot.Meshes[0].Entity });
				fixture.Submit(snapshot);
				const std::vector<glm::dvec4> view = ReadHalfTexels(device, *fixture.Renderer().GetSceneColorTexture());
				const RenderStats skyStats = fixture.Renderer().GetRenderStats();
				REQUIRE(skyStats.PlanarReflections == 1);
				const RenderPassStats* row = Test::SceneRendererIntegrationFixture::FindPass(skyStats, "PlanarReflection0");
				REQUIRE(row != nullptr);
				const uint32_t skyDrawCalls = row->DrawCalls;

				// The mirror camera's own view of the sky (no glass, no reflection): the glass's pixel (x, y) is its (Size - 1 - x, y).
				RenderSnapshot mirrored = snapshot;
				mirrored.Camera = MakePlanarReflectionCamera(snapshot.Camera, PlanarReflectionPlane{ .Normal = glm::vec3(0.0f, 0.0f, 1.0f), .Distance = 3.0f }, Size, Size);
				mirrored.Meshes.clear();
				mirrored.PlanarReflections.clear();
				fixture.Submit(mirrored);
				const std::vector<glm::dvec4> mirrorView = ReadHalfTexels(device, *fixture.Renderer().GetSceneColorTexture());
				uint32_t mismatched = 0;
				for (uint32_t y = 18; y < 46; ++y)
				{
					for (uint32_t x = 18; x < 46; ++x)
					{
						const glm::dvec3 glass(view[static_cast<size_t>(y) * Size + x]);
						const glm::dvec3 sky(mirrorView[static_cast<size_t>(y) * Size + (Size - 1 - x)]);
						const glm::dvec3 difference = glm::abs(glass - sky);
						if (std::max({ difference.r, difference.g, difference.b }) > 2e-2 * std::max({ sky.r, sky.g, sky.b }) + 1e-3)
							++mismatched;
					}
				}
				CHECK(mismatched <= 8);
				// Above the glass's centre it shows the blue sky a few degrees above the horizon behind the camera.
				const glm::dvec3 above(view[static_cast<size_t>(Size / 2 - 8) * Size + Size / 2]);
				CHECK(above.b > above.r);
				CHECK(above.r > 0.0);

				// Without the Sky the mirror view draws nothing over its cleared pixels: one draw fewer.
				RenderSnapshot plain = snapshot;
				plain.Sky.Enabled = false;
				fixture.Submit(plain);
				const RenderStats plainStats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* plainRow = Test::SceneRendererIntegrationFixture::FindPass(plainStats, "PlanarReflection0");
				REQUIRE(plainRow != nullptr);
				CHECK(skyDrawCalls == plainRow->DrawCalls + 1);

				// A probe capture of the sky scene (no mirror), the sun low ahead so that no face above sees its halo: its +Y face
				// sees the blue sky, its sky drawn from the probe's position; without the Sky the clear colour, black.
				Result<Scope<ProbeCapture>> capture = ProbeCapture::Create(device, fixture.Pipelines(), fixture.Cache(), fixture.Assets());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
				RenderSnapshot probe = MakeSkyViewSnapshot(fixture, 10.0f);
				const Result<ProbeCubeCapture> captured = (*capture)->Capture(probe, glm::vec3(0.0f, 1.0f, 0.0f), 16);
				REQUIRE_MESSAGE(captured.has_value(), captured.error().ToString());
				for (const glm::vec4& texel : captured->Faces[2])
				{
					CHECK(texel.r > 0.0f);
					CHECK(texel.b > texel.r);
				}
				probe.Sky.Enabled = false;
				const Result<ProbeCubeCapture> unlit = (*capture)->Capture(probe, glm::vec3(0.0f, 1.0f, 0.0f), 16);
				REQUIRE_MESSAGE(unlit.has_value(), unlit.error().ToString());
				for (const glm::vec4& texel : unlit->Faces[2])
					CHECK(glm::vec3(texel) == glm::vec3(0.0f));
			}
			device.RunGarbageCollection();
		}

		// The view's sky resources live in the renderer's state (place 2): a device loss drops them with it, and the next
		// render creates them again and regenerates the LUTs and the image-based light (their keys are empty), so the image is
		// the same, byte for byte.
		TEST_CASE("SkyLighting: a sky scene renders the same bytes after a device loss" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
			RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, 20.0f);
			snapshot.Sky.Clouds = 0.5f;
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
			// The recovered renderer draws the sky (the byte comparison above proves its light regenerated to the same bytes).
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "Sky") != nullptr);
		}

		// Decision 3, "Device loss, trim and out of memory": a view whose sky resources cannot be created (its cubes out of
		// memory) lights with its Environment exactly as a view without a Sky does, records none of the sky's rows, and its
		// render returns the error (for which the host trims and renders again); the next render creates them and draws the sky.
		TEST_CASE("SkyLighting: a sky whose cubes cannot be created lights with the Environment and the render returns its error"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const auto makeSnapshot = [](Test::SceneRendererIntegrationFixture& fixture)
			{
				RenderSnapshot snapshot = MakeSkyViewSnapshot(fixture, 30.0f);
				snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = true, .FallbackColor = glm::vec3(0.2f, 0.3f, 0.4f) };
				return snapshot;
			};
			std::vector<std::byte> reference;
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
				RenderSnapshot withoutSky = makeSnapshot(fixture);
				withoutSky.Sky.Enabled = false;
				reference = fixture.Render(withoutSky).Pixels;
			}
			{
				Test::SceneRendererIntegrationFixture fixture(gpu, 32, 32);
				RenderSnapshot snapshot = makeSnapshot(fixture);
				{
					const Test::TextureCreationFails failing(device, [](const nvrhi::TextureDesc& desc)
					{
						return desc.dimension == nvrhi::TextureDimension::TextureCube && desc.isUAV;
					});
					Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
					REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
					(*list)->open();
					const Status rendered = fixture.Renderer().Render(**list, snapshot);
					(*list)->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(**list));
					REQUIRE_FALSE(rendered.has_value());
					CHECK(rendered.error().GetCode() == ErrorCode::Gpu);
					CHECK(failing.GetFailedCount() == 1);
					CHECK(fixture.Read().Pixels == reference);
					const RenderStats& stats = fixture.Renderer().GetRenderStats();
					CHECK(stats.SkyUpdates == 0U);
					CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SkyLuts") == nullptr);
					CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "Sky") == nullptr);
				}
				++snapshot.FrameIndex;
				CHECK(fixture.Render(snapshot).Pixels != reference);
				CHECK(fixture.Renderer().GetRenderStats().SkyUpdates == 1U);
				CHECK(Test::SceneRendererIntegrationFixture::FindPass(fixture.Renderer().GetRenderStats(), "Sky") != nullptr);
				CHECK_FALSE(device.IsLost());
			}
			device.RunGarbageCollection();
		}

		// The W45.2 review's finding 11: without a Sun light, ApplySkyLightDriving drives the Moon light as a full moon (k 1), and
		// the sky's constants agree: the moon's fraction is 1 and its disc is lit from behind the viewer (SunDirection opposite
		// the moon), while no sun scatters or shines. With a Sun light, the fraction follows the elongation.
		TEST_CASE("SkyLighting: without a Sun light, the sky's moon is full, as its driven light is")
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			const glm::vec3 toMoon = glm::normalize(glm::vec3(0.0f, 0.42f, -0.9f));
			snapshot.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
				.Color = glm::vec3(0.8f, 0.85f, 1.0f),
				.Intensity = 0.2f,
				.Direction = -toMoon,
				.Entity = UUID(78) });
			snapshot.Sky.Enabled = true;
			snapshot.Sky.Sun = UUID(5150);
			snapshot.Sky.Moon = UUID(78);
			ResolveSkyLights(snapshot);
			REQUIRE(snapshot.Sky.MoonLight.has_value());
			REQUIRE(snapshot.Sky.HasMoon);
			snapshot.Sky.SunLight.reset();
			const SkyConstants moonOnly = MakeSkyConstants(snapshot.Sky, snapshot.Lights, snapshot.Camera, snapshot.Fog);
			CHECK(moonOnly.HasMoon == 1U);
			CHECK(moonOnly.MoonFraction == 1.0f);
			CHECK(glm::dot(moonOnly.SunDirection, moonOnly.MoonDirection) == doctest::Approx(-1.0));
			CHECK(moonOnly.SunIlluminance == glm::vec3(0.0f));
			CHECK(moonOnly.SunDiscRadiance == 0.0f);
			// With a Sun light 30 degrees above the moon, the fraction is its elongation's.
			const RenderSnapshot sunlit = MakeSkySnapshot(60.0f);
			snapshot.Lights.push_back(sunlit.Lights[0]);
			snapshot.Sky.SunLight = static_cast<uint32_t>(snapshot.Lights.size() - 1);
			snapshot.Sky.SunLightColor = sunlit.Sky.SunLightColor;
			const SkyConstants withSun = MakeSkyConstants(snapshot.Sky, snapshot.Lights, snapshot.Camera, snapshot.Fog);
			const glm::dvec3 toSun = -glm::dvec3(sunlit.Lights[0].Direction);
			CHECK(withSun.MoonFraction == doctest::Approx(ComputeMoonIlluminatedFraction(glm::normalize(toSun), glm::dvec3(withSun.MoonDirection))).epsilon(1e-5));
			CHECK(withSun.MoonFraction < 0.5f);
		}

		// The W45.2 review's nits 12 and 13: the moon hides the stars behind it, its dark limb included (a 10-degree moon filling
		// the view's centre renders the same bytes there with and without stars, which show around it), and an orthographic
		// view, whose one direction would spread a disc or a star over the whole background, draws none: looking along the sun,
		// its background is the sky's, far below the disc's radiance.
		TEST_CASE("SkyPass: the moon hides the stars behind it, and an orthographic view draws no disc or star" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			constexpr uint32_t Size = 64;
			Test::SceneRendererIntegrationFixture fixture(gpu, Size, Size);
			// The camera turned 30 degrees up, toward the moon (or the sun) 30 degrees above -Z.
			const glm::vec3 up30 = glm::normalize(glm::vec3(0.0f, 0.5f, -0.8660254f));
			const glm::mat4 view = glm::rotate(glm::mat4(1.0f), glm::radians(-30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			{
				RenderSnapshot night = MakeSkyViewSnapshot(fixture, -30.0f);
				night.Meshes.clear();
				night.Camera.View = view;
				night.Camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 20.0f, 2.0f, 0.1f, 1000.0f, Size, Size);
				night.Lights.push_back(LightData{ .Type = RenderLightType::Directional,
					.Color = glm::vec3(0.8f, 0.85f, 1.0f),
					.Intensity = 0.2f,
					.Direction = -up30,
					.Entity = UUID(78) });
				night.Sky.Moon = UUID(78);
				night.Sky.MoonSize = 10.0f;
				ResolveSkyLights(night);
				REQUIRE(night.Sky.HasMoon);
				const auto render = [&fixture, &device, &night](float stars)
				{
					night.Sky.Stars = stars;
					++night.FrameIndex;
					fixture.Submit(night);
					return ReadHalfTexels(device, *fixture.Renderer().GetSceneColorTexture());
				};
				const std::vector<glm::dvec4> dark = render(0.0f);
				const std::vector<glm::dvec4> starry = render(100.0f);
				// Within 12 pixels of the centre (3.75 degrees at 20 degrees over 64 pixels) is the moon's disc of 5 degrees' radius.
				uint32_t insideDiffers = 0;
				uint32_t outsideDiffers = 0;
				for (uint32_t y = 0; y < Size; ++y)
				{
					for (uint32_t x = 0; x < Size; ++x)
					{
						const int dx = static_cast<int>(x) - static_cast<int>(Size / 2);
						const int dy = static_cast<int>(y) - static_cast<int>(Size / 2);
						const size_t index = static_cast<size_t>(y) * Size + x;
						const bool differs = dark[index] != starry[index];
						if (dx * dx + dy * dy <= 12 * 12)
							insideDiffers += differs ? 1U : 0U;
						else if (dx * dx + dy * dy > 24 * 24)
							outsideDiffers += differs ? 1U : 0U;
					}
				}
				CHECK(insideDiffers == 0U);
				CHECK(outsideDiffers > 0U);
			}
			{
				// Orthographic, along the sun 30 degrees up: every pixel takes that one direction.
				RenderSnapshot ortho = MakeSkyViewSnapshot(fixture, 30.0f);
				ortho.Meshes.clear();
				ortho.Camera.ProjectionKind = RenderProjection::Orthographic;
				ortho.Camera.Projection = ComputeReverseZProjection(RenderProjection::Orthographic, 60.0f, 2.0f, 0.1f, 1000.0f, Size, Size);
				ortho.Camera.View = view;
				fixture.Submit(ortho);
				const std::vector<glm::dvec4> texels = ReadHalfTexels(device, *fixture.Renderer().GetSceneColorTexture());
				const SkyConstants constants = MakeSkyConstants(ortho.Sky, ortho.Lights, ortho.Camera, ortho.Fog);
				REQUIRE(constants.SunDiscRadiance > 0.0f);
				INFO("background " << texels[0].g << ", the disc's radiance per unit illuminance " << constants.SunDiscRadiance);
				for (const glm::dvec4& texel : texels)
					CHECK(texel.g < 0.01 * static_cast<double>(constants.SunDiscRadiance));
			}
			device.RunGarbageCollection();
		}
	}

}
