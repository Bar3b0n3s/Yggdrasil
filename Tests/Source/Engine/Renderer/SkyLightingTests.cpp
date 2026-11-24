#include "TestsPCH.h"

#include "Engine/Renderer/SkyLighting.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Shared/EnvironmentConstants.h"
#include "Shared/FogConstants.h"
#include "Shared/ProbeConstants.h"
#include "Shared/SkyConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/SkyCopyProgram.h"

#include <glm/glm.hpp>

#include <array>
#include <cstring>
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
				{ .bindings = { nvrhi::BindingSetItem::ConstantBuffer(1, sky), nvrhi::BindingSetItem::StructuredBuffer_UAV(6, results) } }, *write->BindingLayouts[0]);
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
																						nvrhi::BindingSetItem::StructuredBuffer_UAV(7, outputs[frameIndex][view]) } },
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
				list.dispatch(1);
				const std::array<nvrhi::IBuffer*, 2> fogs = { fog.Get(), mirrorFog.Get() };
				RecordSkyResultCopies(list, results, environment, fogs, probes, CopiedReflectionProbes);
				for (size_t view = 0; view < 2; ++view)
				{
					nvrhi::ComputeState readState;
					readState.pipeline = read->Pipeline;
					readState.bindings = { readSets[frameIndex][view] };
					list.setComputeState(readState);
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
	}

}
