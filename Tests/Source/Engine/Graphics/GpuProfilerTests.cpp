#include "TestsPCH.h"

#include "Engine/Graphics/GpuProfiler.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuProfiler: timed scopes report their names, nesting and times FramesInFlight frames later"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FramePacer pacer(device, 2);
			GpuProfiler profiler(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			for (int frame = 0; frame < 4; ++frame)
			{
				pacer.BeginFrame();
				profiler.BeginFrame(pacer.GetFrameSlot());
				(*commandList)->open();
				{
					GpuProfileScope outer(profiler, **commandList, "Outer");
					GpuProfileScope inner(profiler, **commandList, "Inner");
				}
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
			}

			// The frames of the earlier slots have completed by now, so their samples were collected.
			const std::span<const GpuTimingSample> samples = profiler.GetLastFrameSamples();
			REQUIRE(samples.size() == 2);
			CHECK(samples[0].Name == "Outer");
			CHECK(samples[0].Depth == 0);
			CHECK(samples[1].Name == "Inner");
			CHECK(samples[1].Depth == 1);
			CHECK(samples[0].Milliseconds >= 0.0);
			CHECK(samples[1].Milliseconds >= 0.0);
			device.WaitForIdle();
		}
	}

}
