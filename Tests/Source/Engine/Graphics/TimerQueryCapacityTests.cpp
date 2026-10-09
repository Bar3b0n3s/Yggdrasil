#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Graphics/GpuProfiler.h"
#include "Support/HeadlessGpuFixture.h"

#include <limits>
#include <vector>

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GraphicsDevice: overflowing timer-query capacity is rejected before Vulkan creation")
		{
			GraphicsDeviceSpecification specification;
			specification.Graphics.FramesInFlight = std::numeric_limits<uint32_t>::max();
			const auto result = GraphicsDevice::Create(specification);
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(result.error().GetMessageText().contains("timer-query capacity"));
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GraphicsDevice: eight profilers retain full per-slot timer capacity without evicting other views")
		{
			// Three slots prevents a fixed two-frame budget from accidentally satisfying the contract.
			Test::HeadlessGpuFixture gpu({ .FramesInFlight = 3 });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			std::vector<Scope<GpuProfiler>> profilers;
			for (uint32_t owner = 0; owner < GraphicsDevice::MaxConcurrentGpuProfilers; ++owner)
				profilers.push_back(CreateScope<GpuProfiler>(device, device.GetFramesInFlight()));
			auto commands = device.CreateCommandList();
			REQUIRE(commands);
			for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
			{
				(*commands)->open();
				for (uint32_t owner = 0; owner < profilers.size(); ++owner)
				{
					GpuProfiler& profiler = *profilers[owner];
					profiler.BeginFrame(slot, 100 * owner + slot);
					for (uint32_t scope = 0; scope < GpuProfiler::MaxScopesPerFrame; ++scope)
					{
						GpuProfileScope timed(profiler, **commands, "Capacity");
					}
				}
				(*commands)->close();
				device.ExecuteCommandList(**commands);
				device.WaitForIdle();
			}
			for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				for (uint32_t owner = 0; owner < profilers.size(); ++owner)
				{
					profilers[owner]->BeginFrame(slot, 10000 + owner * 100 + slot);
					const auto result = profilers[owner]->GetLastFrameResult();
					REQUIRE(result.Available);
					CHECK(result.FrameIndex == 100 * owner + slot);
					CHECK(result.Samples.size() == GpuProfiler::MaxScopesPerFrame);
				}
			profilers.clear();
			device.RunGarbageCollection();
			// Transient captures/thumbnails can be replaced without exhausting retired queries.
			GpuProfiler replacement(device, device.GetFramesInFlight());
			replacement.BeginFrame(0, 90000);
			(*commands)->open();
			for (uint32_t scope = 0; scope < GpuProfiler::MaxScopesPerFrame; ++scope)
			{
				GpuProfileScope timed(replacement, **commands, "Replacement");
			}
			(*commands)->close();
			device.ExecuteCommandList(**commands);
			device.WaitForIdle();
			replacement.BeginFrame(0, 90001);
			CHECK(replacement.GetLastFrameResult().Available);
			CHECK(replacement.GetLastFrameResult().Samples.size() == GpuProfiler::MaxScopesPerFrame);
		}
	}

}
