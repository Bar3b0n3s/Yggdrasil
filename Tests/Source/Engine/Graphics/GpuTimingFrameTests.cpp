#include "TestsPCH.h"

#include "Engine/Graphics/GpuProfiler.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

#include <array>

namespace Engine {

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GpuProfiler: delayed samples retain the identity of their recorded frame")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuProfiler profiler(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			constexpr std::array<uint64_t, 4> FrameIds = { 41, 103, 777, 2049 };
			GpuTimingFrame firstResult{};

			for (uint32_t index = 0; index < FrameIds.size(); ++index)
			{
				profiler.BeginFrame(index % 2, FrameIds[index]);
				const GpuTimingFrame result = profiler.GetLastFrameResult();
				if (index < 2)
				{
					CHECK_FALSE(result.Available);
					CHECK(result.Samples.empty());
				}
				else
				{
					REQUIRE(result.Available);
					CHECK(result.FrameIndex == FrameIds[index - 2]);
					REQUIRE(result.Samples.size() == 1);
					CHECK(result.Samples.front().Name == "Recorded");
					CHECK(result.Samples.front().Milliseconds >= 0.0);
					if (index == 2)
						firstResult = result;
				}
				(*commandList)->open();
				{
					GpuProfileScope scope(profiler, **commandList, "Recorded");
				}
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);
				// Deliberately retire each submission; the profiler itself must never wait.
				device.WaitForIdle();
			}
			CHECK(firstResult.Available);
			CHECK(firstResult.FrameIndex == FrameIds[0]);
			REQUIRE(firstResult.Samples.size() == 1);
			CHECK(firstResult.Samples.front().Name == "Recorded");
		}

		TEST_CASE("GpuProfiler: an unavailable collection after a success exposes no relabelled samples")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuProfiler profiler(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			profiler.BeginFrame(0, 17);
			(*commandList)->open();
			{
				GpuProfileScope scope(profiler, **commandList, "Completed");
			}
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);
			device.WaitForIdle();

			profiler.BeginFrame(0, 50);
			const GpuTimingFrame completed = profiler.GetLastFrameResult();
			REQUIRE(completed.Available);
			CHECK(completed.FrameIndex == 17);
			REQUIRE(completed.Samples.size() == 1);
			CHECK(completed.Samples.front().Name == "Completed");

			// Slot one has never recorded a frame: it cannot publish slot zero's prior success.
			profiler.BeginFrame(1, 900);
			const GpuTimingFrame unavailable = profiler.GetLastFrameResult();
			CHECK_FALSE(unavailable.Available);
			CHECK(unavailable.Samples.empty());
			CHECK(profiler.GetLastFrameSamples().empty());
			CHECK(completed.FrameIndex == 17);
			CHECK(completed.Samples.size() == 1);
		}
	}

}
