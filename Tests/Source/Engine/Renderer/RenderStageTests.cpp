#include "TestsPCH.h"

#include "Engine/Renderer/RenderStage.h"

#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The GPU-zone hook at each stage boundary and the profiler's GPU merge (M18, C18.1, R-PASS and R-PROF;
// Docs/Decisions/0029-m18-contract.md decisions 4 and 11). Skipped until stream C implements them.

namespace Engine {

	namespace {

		// The names of the zones of `zones` on the CPU threads (not Profiler::GpuThreadIndex) that start with "Stage:", in
		// timeline order.
		std::vector<std::string> GetCpuStageZoneNames(const std::vector<ProfileZone>& zones)
		{
			std::vector<std::string> names;
			for (const ProfileZone& zone : zones)
			{
				if (zone.ThreadIndex != Profiler::GpuThreadIndex && zone.Name.starts_with("Stage:"))
					names.emplace_back(zone.Name);
			}
			return names;
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("RenderRecordingContext: each stage is one GPU zone around its passes" * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			GraphicsDevice& device = gpu.GetDevice();
			GpuProfiler profiler(device, 1);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			RenderStats stats{ .Available = true, .FrameIndex = 5 };
			double nowSeconds = 0.0;
			const uint64_t since = Profiler::GetTimeNs();
			{
				RenderRecordingContext recording(stats, [&nowSeconds]()
				{
					return nowSeconds;
				}, &profiler);
				profiler.BeginFrame(0, 5);
				(*commandList)->open();
				// Prepare records no GPU work: its stage has no command list.
				recording.BeginStage(RenderStage::Prepare);
				recording.BeginPass("Prepare");
				nowSeconds += 0.001;
				recording.EndPass({});
				recording.EndStage();
				// A stage that records no pass still opens and closes its zone.
				recording.BeginStage(RenderStage::Shadows, *commandList);
				recording.EndStage();
				recording.BeginStage(RenderStage::Forward, *commandList);
				recording.BeginPass("ForwardOpaque", *commandList);
				recording.EndPass({ .DrawCalls = 1 });
				recording.BeginPass("Skybox", *commandList);
				recording.EndPass({ .DrawCalls = 1 });
				recording.EndStage();
				(*commandList)->close();
			}
			device.ExecuteCommandList(**commandList);
			device.WaitForIdle();
			profiler.BeginFrame(0, 6);
			const GpuTimingFrame result = profiler.GetLastFrameResult();
			REQUIRE(result.Available);
			REQUIRE(result.Samples.size() == 4);
			CHECK(result.Samples[0].Name == "Stage:Shadows");
			CHECK(result.Samples[0].Depth == 0);
			CHECK(result.Samples[1].Name == "Stage:Forward");
			CHECK(result.Samples[1].Depth == 0);
			CHECK(result.Samples[2].Name == "ForwardOpaque");
			CHECK(result.Samples[2].Depth == 1);
			CHECK(result.Samples[3].Name == "Skybox");
			CHECK(result.Samples[3].Depth == 1);
			// A stage is never a stats row.
			REQUIRE(stats.Passes.size() == 3);
			CHECK(stats.Passes[0].Name == "Prepare");
			CHECK(stats.Passes[1].Name == "ForwardOpaque");
			CHECK(stats.Passes[2].Name == "Skybox");
			// Every stage, Prepare's included, is a CPU zone of the process profiler.
			CHECK(GetCpuStageZoneNames(Profiler::CollectZones(since)) == std::vector<std::string>{ "Stage:Prepare", "Stage:Shadows", "Stage:Forward" });
		}

		TEST_CASE("Profiler: a rendered view's GPU zones reach the profiler timeline" * doctest::skip(true))
		{
			// R-PROF: the per-pass and per-stage GPU timings of a view are merged into the process profiler's timeline (on the
			// GPU thread index) once their frame retires, so profiler.capture's Chrome trace has CPU and GPU zones.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			REQUIRE(Profiler::IsInitialized());
			Test::SceneRendererIntegrationFixture setup(gpu);
			const uint64_t since = Profiler::GetTimeNs();
			RenderSnapshot snapshot = setup.Snapshot();
			// The renderer collects a frame's timings when its timing slot comes round again.
			for (uint32_t frame = 0; frame <= gpu.GetDevice().GetFramesInFlight(); ++frame)
			{
				snapshot.FrameIndex = 100 + frame;
				setup.Submit(snapshot);
				gpu.GetDevice().WaitForIdle();
			}
			const std::vector<ProfileZone> zones = Profiler::CollectZones(since);
			const auto isGpuZone = [](std::string_view name)
			{
				return [name](const ProfileZone& zone)
				{
					return zone.ThreadIndex == Profiler::GpuThreadIndex && zone.Name == name;
				};
			};
			const auto forward = std::find_if(zones.begin(), zones.end(), isGpuZone("ForwardOpaque"));
			const auto stage = std::find_if(zones.begin(), zones.end(), isGpuZone("Stage:Forward"));
			REQUIRE(forward != zones.end());
			REQUIRE(stage != zones.end());
			CHECK(forward->BeginNs >= since);
			CHECK(stage->BeginNs <= forward->BeginNs);
			CHECK(forward->EndNs <= stage->EndNs);
			CHECK(forward->Depth == stage->Depth + 1);
			// Each retired frame is merged once.
			CHECK(static_cast<uint32_t>(std::count_if(zones.begin(), zones.end(), isGpuZone("Stage:Forward"))) <= gpu.GetDevice().GetFramesInFlight());
		}
	}

}
