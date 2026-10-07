#include "TestsPCH.h"

#include "Engine/Graphics/GpuProfiler.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuProfiler: timed scopes report their names, nesting and times FramesInFlight frames later"
			* doctest::test_suite(Test::GpuSuite))
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

		TEST_CASE("GpuProfiler: scopes are laid out back to back on the frame's GPU timeline" * doctest::test_suite(Test::GpuSuite))
		{
			// Timer queries measure durations, so a scope starts where its previous sibling ended, or where its parent started
			// (GpuTimingSample::StartMilliseconds), from 0 at the frame's first scope.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FramePacer pacer(device, 2);
			GpuProfiler profiler(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			for (int frame = 0; frame < 3; ++frame)
			{
				pacer.BeginFrame();
				profiler.BeginFrame(pacer.GetFrameSlot());
				(*commandList)->open();
				{
					GpuProfileScope first(profiler, **commandList, "First");
					{
						GpuProfileScope childA(profiler, **commandList, "ChildA");
					}
					{
						GpuProfileScope childB(profiler, **commandList, "ChildB");
					}
				}
				{
					GpuProfileScope second(profiler, **commandList, "Second");
				}
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
			}

			// The third frame's BeginFrame collected the first frame.
			const std::span<const GpuTimingSample> samples = profiler.GetLastFrameSamples();
			REQUIRE(samples.size() == 4);
			CHECK(samples[0].Name == "First");
			CHECK(samples[1].Name == "ChildA");
			CHECK(samples[2].Name == "ChildB");
			CHECK(samples[3].Name == "Second");
			CHECK(samples[0].Depth == 0);
			CHECK(samples[1].Depth == 1);
			CHECK(samples[2].Depth == 1);
			CHECK(samples[3].Depth == 0);
			CHECK(samples[0].StartMilliseconds == 0.0);
			CHECK(samples[1].StartMilliseconds == 0.0);
			CHECK(samples[2].StartMilliseconds == doctest::Approx(samples[1].StartMilliseconds + samples[1].Milliseconds));
			CHECK(samples[3].StartMilliseconds == doctest::Approx(samples[0].StartMilliseconds + samples[0].Milliseconds));
			device.WaitForIdle();
		}

		TEST_CASE("GpuProfiler: two profilers time MaxScopesPerFrame scopes in every one of three frames in flight"
			* doctest::test_suite(Test::GpuSuite))
		{
			// The device sizes NVRHI's timer-query pool for an application profiler and a capture's at their fullest
			// (GraphicsDevice.cpp); NVRHI's default pool of 256 queries would run out in the third frame here and report an
			// error, which the fixture's destruction check fails on.
			constexpr uint32_t FramesInFlight = 3;
			Test::HeadlessGpuFixture gpu({ .FramesInFlight = FramesInFlight });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FramePacer pacer(device, FramesInFlight);
			GpuProfiler application(device, FramesInFlight);
			GpuProfiler capture(device, FramesInFlight);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			// One more frame than there are slots, so the first slot's frame is collected.
			for (uint32_t frame = 0; frame <= FramesInFlight; ++frame)
			{
				pacer.BeginFrame();
				application.BeginFrame(pacer.GetFrameSlot());
				capture.BeginFrame(pacer.GetFrameSlot());
				(*commandList)->open();
				for (GpuProfiler* profiler : { &application, &capture })
				{
					for (uint32_t scope = 0; scope < GpuProfiler::MaxScopesPerFrame; ++scope)
						GpuProfileScope timed(*profiler, **commandList, "Scope");
				}
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
			}

			CHECK(application.GetLastFrameSamples().size() == GpuProfiler::MaxScopesPerFrame);
			CHECK(capture.GetLastFrameSamples().size() == GpuProfiler::MaxScopesPerFrame);
			device.WaitForIdle();
		}
	}

}
